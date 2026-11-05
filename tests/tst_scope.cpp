#include "scopeworker.h"
#include "mainwindow.h"

#include <QBuffer>
#include <QHostAddress>
#include <QPainter>
#include <QCheckBox>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QTemporaryDir>
#include <memory>

namespace {

QImage testImage()
{
    QImage image(320, 200, QImage::Format_RGB32);
    image.fill(QColor(18, 27, 43));
    QPainter painter(&image);
    painter.setPen(QColor(49, 67, 91));
    for (int x = 0; x < image.width(); x += 20)
        painter.drawLine(x, 0, x, image.height());
    for (int y = 0; y < image.height(); y += 20)
        painter.drawLine(0, y, image.width(), y);
    painter.fillRect(52, 74, 80, 40, QColor(247, 194, 67));
    painter.setPen(Qt::white);
    painter.drawText(12, 185, QStringLiteral("LOCAL TEST / NOT AN INSTRUMENT CAPTURE"));
    return image;
}

QByteArray testPng()
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!testImage().save(&buffer, "PNG"))
        qFatal("Cannot encode local PNG fixture");
    return bytes;
}

class WorkerHarness
{
public:
    WorkerHarness()
    {
        worker = new ScopeWorker;
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();
    }
    ~WorkerHarness()
    {
        QMetaObject::invokeMethod(worker, &ScopeWorker::disconnectDevice, Qt::BlockingQueuedConnection);
        thread.quit();
        if (!thread.wait(15000))
            qFatal("Scope worker failed to stop within its bounded I/O deadline");
    }
    ScopeWorker *worker = nullptr;
    QThread thread;
};

class FakeScpiServer : public QObject
{
public:
    enum Mode { Normal, TruncatedImage, WrongIdentity };

    explicit FakeScpiServer(Mode mode = Normal) : mode(mode)
    {
        QObject::connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
                auto pending = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket, pending] {
                    pending->append(socket->readAll());
                    int newline;
                    while ((newline = pending->indexOf('\n')) >= 0) {
                        const auto command = pending->left(newline).trimmed();
                        pending->remove(0, newline + 1);
                        commands.append(command.toUpper());
                        if (command == "*IDN?") {
                            const QByteArray identity = this->mode == WrongIdentity
                                ? "RIGOL TECHNOLOGIES,DS1054Z,LOCALTEST,00.01\n"
                                : "Siglent Technologies,SDS3054X HD,LOCALTEST,1.0.4.2\n";
                            sendFragmented(socket, identity, false);
                        } else if (command.toUpper() == ":PRINT? PNG") {
                            auto response = testPng();
                            if (this->mode == TruncatedImage)
                                response.chop(13);
                            sendFragmented(socket, response, this->mode == TruncatedImage);
                        } else {
                            socket->write("UNEXPECTED COMMAND\n");
                        }
                    }
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        if (!server.listen(QHostAddress::LocalHost, 0))
            qFatal("Cannot start local SCPI test server");
    }

    quint16 port() const { return server.serverPort(); }
    QList<QByteArray> commands;

private:
    void sendFragmented(QTcpSocket *socket, const QByteArray &bytes, bool closeAfter)
    {
        const QList<int> firstSizes{1, 2, 5, 13, 31, 257};
        qsizetype position = 0;
        int tick = 0;
        while (position < bytes.size()) {
            const auto count = qMin<qsizetype>(
                tick < firstSizes.size() ? firstSizes.at(tick) : 4096,
                bytes.size() - position);
            const auto part = bytes.mid(position, count);
            QTimer::singleShot(tick * 6, socket, [socket, part] {
                socket->write(part);
                socket->flush();
            });
            position += count;
            ++tick;
        }
        if (closeAfter)
            QTimer::singleShot(tick * 6 + 5, socket, &QTcpSocket::disconnectFromHost);
    }

    Mode mode;
    QTcpServer server;
};

} // namespace

class ScopeTests : public QObject
{
    Q_OBJECT
    QTemporaryDir settingsDirectory;
private slots:
    void initTestCase()
    {
        QVERIFY(settingsDirectory.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("ScopeSnapOfflineTests"));
        QCoreApplication::setApplicationName(QStringLiteral("ScopeSnapOfflineTests"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
        qRegisterMetaType<QImage>();
        qRegisterMetaType<QStringList>();
    }

    void pngRoundTrip()
    {
        QImage decoded;
        QString error;
        QVERIFY2(ScopeWorker::decodeScreenshot(testPng(), &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.size(), testImage().size());
        QCOMPARE(decoded.convertToFormat(QImage::Format_RGB32), testImage());
    }

    void damagedPng_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        const QByteArray valid = testPng();
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("text-response") << QByteArray("SCPI ERROR\n");
        QTest::newRow("signature-only") << valid.left(8);
        QTest::newRow("partial-header") << valid.left(20);
        QTest::newRow("missing-iend") << valid.left(valid.size() - 12);
        QTest::newRow("truncated-iend") << valid.left(valid.size() - 3);
        auto badCrc = valid;
        badCrc[29] = char(badCrc.at(29) ^ 0x01);
        QTest::newRow("corrupt-ihdr-crc") << badCrc;
        auto enormousChunk = valid;
        enormousChunk.replace(8, 4, QByteArray::fromHex("7fffffff"));
        QTest::newRow("oversized-chunk") << enormousChunk;
        QTest::newRow("unexpected-trailer") << (valid + QByteArray("invalid trailing data"));
    }

    void damagedPng()
    {
        QFETCH(QByteArray, bytes);
        QImage decoded;
        QString error;
        QVERIFY(!ScopeWorker::decodeScreenshot(bytes, &decoded, &error));
        QVERIFY2(!error.isEmpty(), "Malformed data should produce a useful error");
    }

    void demoLifecycle()
    {
        ScopeWorker worker;
        QSignalSpy connected(&worker, &ScopeWorker::connected);
        QSignalSpy captured(&worker, &ScopeWorker::captured);
        QSignalSpy errors(&worker, &ScopeWorker::error);
        QSignalSpy finished(&worker, &ScopeWorker::operationFinished);
        worker.useDemo();
        QCOMPARE(connected.count(), 1);
        QVERIFY(connected.first().at(2).toBool());
        worker.capture();
        QCOMPARE(captured.count(), 1);
        QVERIFY(captured.first().at(1).toBool());
        const auto image = qvariant_cast<QImage>(captured.first().at(0));
        QVERIFY(image.width() >= 1000);
        QVERIFY(image.height() >= 600);
        QCOMPARE(errors.count(), 0);
        worker.disconnectDevice();
        worker.capture();
        QCOMPARE(errors.count(), 1);
        QCOMPARE(captured.count(), 1);
        QCOMPARE(finished.count(), 4);
    }

    void lanFragmentedCaptureAndRepeat()
    {
        FakeScpiServer server;
        const quint16 port = server.port();
        WorkerHarness harness;
        auto *worker = harness.worker;
        QSignalSpy connected(worker, &ScopeWorker::connected);
        QSignalSpy captured(worker, &ScopeWorker::captured);
        QSignalSpy errors(worker, &ScopeWorker::error);
        QSignalSpy finished(worker, &ScopeWorker::operationFinished);
        QMetaObject::invokeMethod(worker, [worker, port] {
            worker->openLan(QStringLiteral("127.0.0.1"), port);
        }, Qt::QueuedConnection);
        QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);
        QVERIFY(!connected.first().at(2).toBool());
        QCOMPARE(connected.first().at(0).toString(), QStringLiteral("SDS3054X HD"));
        for (int capture = 1; capture <= 2; ++capture) {
            QMetaObject::invokeMethod(worker, &ScopeWorker::capture, Qt::QueuedConnection);
            QTRY_COMPARE_WITH_TIMEOUT(captured.count(), capture, 5000);
            QVERIFY(!captured.last().at(1).toBool());
            QCOMPARE(qvariant_cast<QImage>(captured.last().at(0)).convertToFormat(QImage::Format_RGB32),
                     testImage());
        }
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 2000);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(server.commands, QList<QByteArray>({"*IDN?", ":PRINT? PNG", ":PRINT? PNG"}));
    }

    void lanInterruptedImageClosesSession()
    {
        FakeScpiServer server(FakeScpiServer::TruncatedImage);
        const quint16 port = server.port();
        WorkerHarness harness;
        auto *worker = harness.worker;
        QSignalSpy connected(worker, &ScopeWorker::connected);
        QSignalSpy captured(worker, &ScopeWorker::captured);
        QSignalSpy errors(worker, &ScopeWorker::error);
        QSignalSpy disconnected(worker, &ScopeWorker::disconnected);
        QMetaObject::invokeMethod(worker, [worker, port] {
            worker->openLan(QStringLiteral("127.0.0.1"), port);
        }, Qt::QueuedConnection);
        QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 5000);
        QMetaObject::invokeMethod(worker, &ScopeWorker::capture, Qt::QueuedConnection);
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 5000);
        QCOMPARE(captured.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(disconnected.count() >= 1, 2000);
        QMetaObject::invokeMethod(worker, &ScopeWorker::capture, Qt::QueuedConnection);
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 2, 2000);
        QCOMPARE(server.commands, QList<QByteArray>({"*IDN?", ":PRINT? PNG"}));
    }

    void lanRejectsOtherManufacturer()
    {
        FakeScpiServer server(FakeScpiServer::WrongIdentity);
        const quint16 port = server.port();
        WorkerHarness harness;
        auto *worker = harness.worker;
        QSignalSpy connected(worker, &ScopeWorker::connected);
        QSignalSpy errors(worker, &ScopeWorker::error);
        QSignalSpy finished(worker, &ScopeWorker::operationFinished);
        QMetaObject::invokeMethod(worker, [worker, port] {
            worker->openLan(QStringLiteral("127.0.0.1"), port);
        }, Qt::QueuedConnection);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(connected.count(), 0);
        QCOMPARE(errors.count(), 1);
        QCOMPARE(server.commands, QList<QByteArray>({"*IDN?"}));
    }

    void uiDemoCaptureSavesAndDisconnects()
    {
        QTemporaryDir output;
        QVERIFY(output.isValid());
        MainWindow window(true);
        auto *capture = window.findChild<QPushButton *>(QStringLiteral("captureButton"));
        auto *save = window.findChild<QPushButton *>(QStringLiteral("saveButton"));
        auto *connection = window.findChild<QPushButton *>(QStringLiteral("connectButton"));
        auto *directory = window.findChild<QLineEdit *>(QStringLiteral("outputDirectory"));
        auto *prefix = window.findChild<QLineEdit *>(QStringLiteral("filenamePrefix"));
        auto *automatic = window.findChild<QCheckBox *>(QStringLiteral("autoSave"));
        auto *history = window.findChild<QListWidget *>(QStringLiteral("historyList"));
        QVERIFY(capture && save && connection && directory && prefix && automatic && history);
        QVERIFY(!capture->isEnabled());
        QVERIFY(!save->isEnabled());
        directory->setText(output.filePath(QStringLiteral("captures")));
        prefix->setText(QStringLiteral("电机启动_24V"));
        automatic->setChecked(true);
        QTRY_VERIFY_WITH_TIMEOUT(capture->isEnabled(), 5000);
        capture->click();
        QTRY_COMPARE_WITH_TIMEOUT(history->count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 2000);
        const QString filename = history->item(0)->data(Qt::UserRole).toString();
        QVERIFY(QFileInfo::exists(filename));
        QVERIFY(QFileInfo(filename).fileName().startsWith(QStringLiteral("演示_电机启动_24V_")));
        QImage stored(filename);
        QCOMPARE(stored.size(), QSize(1280, 800));
        connection->click();
        QTRY_VERIFY_WITH_TIMEOUT(!capture->isEnabled(), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(connection->text(), QStringLiteral("开始演示"), 2000);
        QVERIFY(save->isEnabled()); // The already captured image remains available.
    }

    void uiSaveFailureIsReported()
    {
        QTemporaryDir output;
        QVERIFY(output.isValid());
        const QString blockerPath = output.filePath(QStringLiteral("not_a_directory"));
        QFile blocker(blockerPath);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.write("test fixture");
        blocker.close();
        MainWindow window(true);
        auto *capture = window.findChild<QPushButton *>(QStringLiteral("captureButton"));
        auto *save = window.findChild<QPushButton *>(QStringLiteral("saveButton"));
        auto *directory = window.findChild<QLineEdit *>(QStringLiteral("outputDirectory"));
        auto *automatic = window.findChild<QCheckBox *>(QStringLiteral("autoSave"));
        auto *history = window.findChild<QListWidget *>(QStringLiteral("historyList"));
        auto *message = window.findChild<QLabel *>(QStringLiteral("messageLabel"));
        QVERIFY(capture && save && directory && automatic && history && message);
        directory->setText(blockerPath);
        automatic->setChecked(true);
        QTRY_VERIFY_WITH_TIMEOUT(capture->isEnabled(), 5000);
        capture->click();
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 5000);
        QCOMPARE(history->count(), 0);
        QVERIFY2(message->text().contains(QStringLiteral("无法创建保存目录")), qPrintable(message->text()));
        QVERIFY(blocker.open(QIODevice::ReadOnly));
        QCOMPARE(blocker.readAll(), QByteArray("test fixture"));
    }
};

QTEST_MAIN(ScopeTests)
#include "tst_scope.moc"
