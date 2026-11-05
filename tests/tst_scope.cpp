#include "scopeworker.h"
#include "mainwindow.h"
#include "imagecanvas.h"

#include <QBuffer>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
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
    void setMode(Mode value) { mode=value; }
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
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
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

    void annotationCoordinates_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("fit-with-margins") << 0;
        QTest::newRow("actual-size") << 1;
        QTest::newRow("actual-scrolled") << 2;
    }

    void annotationCoordinates()
    {
        QFETCH(int,mode);
        QScrollArea scroll;
        scroll.resize(630,420);scroll.setWidgetResizable(true);
        auto *canvas=new ImageCanvas;scroll.setWidget(canvas);
        QImage source(1200,800,QImage::Format_RGB32);source.fill(QColor("#202838"));
        canvas->setImage(source);canvas->setActual(mode!=0);scroll.show();
        QTest::qWait(20);
        if(mode==2){scroll.horizontalScrollBar()->setValue(250);scroll.verticalScrollBar()->setValue(170);}
        const QPointF position=mode==2?QPointF(430,330):QPointF(170,130);
        QSignalSpy placed(canvas,&ImageCanvas::addRequested);
        connect(canvas,&ImageCanvas::addRequested,canvas,[canvas](const QPointF &point){canvas->addText(QStringLiteral("峰值\n24 V"),point,24,Qt::yellow);});
        canvas->setAddMode(true);
        QTest::mouseClick(canvas,Qt::LeftButton,Qt::NoModifier,QPoint(3,3));
        QCOMPARE(placed.count(),0);
        const auto viewportPoint=canvas->mapTo(scroll.viewport(),canvas->imageToWidget(position).toPoint());
        QVERIFY(scroll.viewport()->rect().contains(viewportPoint));
        const auto localPoint=canvas->mapFrom(scroll.viewport(),viewportPoint);
        QTest::mouseClick(canvas,Qt::LeftButton,Qt::NoModifier,localPoint);
        QCOMPARE(placed.count(),1);QCOMPARE(canvas->annotations().size(),1);
        const auto actual=canvas->annotations()[0].position;
        QVERIFY(qAbs(actual.x()-position.x())<2.5);QVERIFY(qAbs(actual.y()-position.y())<2.5);
        const auto result=canvas->renderedImage().convertToFormat(QImage::Format_RGB32);
        QCOMPARE(result.size(),source.size());QVERIFY(result!=source);
        QRect changed;
        for(int y=0;y<source.height();++y)for(int x=0;x<source.width();++x)
            if(source.pixel(x,y)!=result.pixel(x,y))changed=changed.united(QRect(x,y,1,1));
        QVERIFY(!changed.isEmpty());QVERIFY(canvas->annotationBounds(0).adjusted(-1,-1,1,1).contains(QRectF(changed)));
        QCOMPARE(canvas->sourceImage(),source);
        canvas->setActual(mode==0);
        QCOMPARE(canvas->renderedImage().convertToFormat(QImage::Format_RGB32),result);
    }

    void annotationDragEditUndoAndBounds()
    {
        ImageCanvas canvas;canvas.resize(800,500);
        QImage source(1200,800,QImage::Format_RGB32);source.fill(QColor("#202838"));
        canvas.setImage(source);canvas.show();
        QVERIFY(canvas.addText(QStringLiteral("启动"),{100,100},28,Qt::yellow));
        const auto first=canvas.renderedImage();
        canvas.markSaved();QVERIFY(!canvas.hasUnsavedChanges());
        const auto press=canvas.imageToWidget(canvas.annotationBounds(0).center());
        const QPointF delta(90,55);
        QTest::mousePress(&canvas,Qt::LeftButton,Qt::NoModifier,press.toPoint());
        QMouseEvent move(QEvent::MouseMove,press+delta,canvas.mapToGlobal((press+delta).toPoint()),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&canvas,&move);
        QTest::mouseRelease(&canvas,Qt::LeftButton,Qt::NoModifier,(press+delta).toPoint());
        const auto moved=canvas.annotations()[0].position;
        const auto rect=canvas.imageRect();
        QVERIFY(qAbs(moved.x()-100-delta.x()*1200/rect.width())<3);
        QVERIFY(qAbs(moved.y()-100-delta.y()*800/rect.height())<3);
        QVERIFY(canvas.hasUnsavedChanges());
        canvas.undo();QCOMPARE(canvas.renderedImage(),first);QVERIFY(!canvas.hasUnsavedChanges());
        QTest::mousePress(&canvas,Qt::LeftButton,Qt::NoModifier,press.toPoint());
        QMouseEvent outward(QEvent::MouseMove,press+delta,canvas.mapToGlobal((press+delta).toPoint()),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&canvas,&outward);
        QMouseEvent back(QEvent::MouseMove,QPointF(press.toPoint()),canvas.mapToGlobal(press.toPoint()),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&canvas,&back);QTest::mouseRelease(&canvas,Qt::LeftButton,Qt::NoModifier,press.toPoint());
        QCOMPARE(canvas.renderedImage(),first);QVERIFY(!canvas.hasUnsavedChanges());
        QVERIFY(canvas.editText(0,QStringLiteral("峰值 18 A\n24 V"),32,Qt::cyan));
        QVERIFY(canvas.renderedImage()!=first);
        QTest::mouseClick(&canvas,Qt::LeftButton,Qt::NoModifier,canvas.imageToWidget(canvas.annotationBounds(0).center()).toPoint());
        QTest::keyClick(&canvas,Qt::Key_Delete);
        QCOMPARE(canvas.renderedImage(),source);
        canvas.undo();QCOMPARE(canvas.annotations().size(),1);
        QVERIFY(canvas.addText(QStringLiteral("右下角"),{1199,799},35,Qt::white));
        QVERIFY(QRectF(source.rect()).contains(canvas.annotationBounds(1)));
        QVERIFY(!canvas.addText(QString(1001,'X'),{0,0},28,Qt::yellow));
        canvas.clearAnnotations();QCOMPARE(canvas.renderedImage(),source);
        canvas.setImage(source);QVERIFY(!canvas.hasUnsavedChanges());QVERIFY(!canvas.canUndo());QCOMPARE(canvas.selectedIndex(),-1);
    }

    void annotationExportPreservesOriginal()
    {
        QTemporaryDir output;QVERIFY(output.isValid());
        MainWindow window(true);window.show();
        auto *canvas=window.findChild<ImageCanvas*>();
        auto *capture=window.findChild<QPushButton*>("captureButton");
        auto *history=window.findChild<QListWidget*>("historyList");
        window.findChild<QLineEdit*>("outputDirectory")->setText(output.path());
        window.findChild<QCheckBox*>("autoSave")->setChecked(true);
        QTRY_VERIFY_WITH_TIMEOUT(capture->isEnabled(),5000);capture->click();
        QTRY_COMPARE_WITH_TIMEOUT(history->count(),1,5000);
        QTRY_VERIFY_WITH_TIMEOUT(canvas->isEnabled(),5000);
        const QString originalPath=history->item(0)->data(Qt::UserRole).toString();
        QFile original(originalPath);QVERIFY(original.open(QIODevice::ReadOnly));const auto originalBytes=original.readAll();original.close();
        const auto originalImage=canvas->sourceImage();
        QVERIFY(canvas->addText(QStringLiteral("峰值 18 A\n24 V · 空载"),{220,140},32,Qt::yellow));
        const auto artifactDirectory=qEnvironmentVariable("SCOPESNAP_TEST_ARTIFACT_DIR");
        if(!artifactDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(artifactDirectory));
            window.findChild<QLineEdit*>("outputDirectory")->setText("C:/Screenshots");
            QApplication::processEvents();
            QVERIFY(window.grab().save(artifactDirectory+"/annotation-ui.png"));
            QVERIFY(canvas->renderedImage().save(artifactDirectory+"/annotation-export.png"));
            window.resize(940,680);QApplication::processEvents();
            QVERIFY(window.grab().save(artifactDirectory+"/annotation-ui-small.png"));
            window.findChild<QLineEdit*>("outputDirectory")->setText(output.path());
        }
        window.findChild<QPushButton*>("copyButton")->click();
        const auto copied=QApplication::clipboard()->image().convertToFormat(QImage::Format_ARGB32);
        QVERIFY(copied!=originalImage.convertToFormat(QImage::Format_ARGB32));QVERIFY(canvas->hasUnsavedChanges());
        window.findChild<QPushButton*>("saveAnnotationsButton")->click();
        QCOMPARE(history->count(),2);QVERIFY(!canvas->hasUnsavedChanges());
        const auto markedPath=history->item(0)->data(Qt::UserRole).toString();
        QVERIFY(markedPath.contains(QStringLiteral("_标注_")));QVERIFY(markedPath!=originalPath);
        QCOMPARE(QImage(markedPath).convertToFormat(QImage::Format_ARGB32),copied);
        QVERIFY(original.open(QIODevice::ReadOnly));QCOMPARE(original.readAll(),originalBytes);
        QCOMPARE(canvas->sourceImage(),originalImage);
        QTimer::singleShot(0,&window,[&]{
            auto *dialog=window.findChild<QFileDialog*>();QVERIFY(dialog);dialog->selectFile(output.filePath("chosen.png"));
            QMetaObject::invokeMethod(dialog,"accept",Qt::DirectConnection);
        });
        window.findChild<QPushButton*>("saveButton")->click();
        QCOMPARE(QImage(output.filePath("chosen.png")).convertToFormat(QImage::Format_ARGB32),copied);
        capture->click();QVERIFY(!canvas->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(canvas->isEnabled(),5000);
        QCOMPARE(canvas->annotations().size(),0);QVERIFY(!canvas->hasUnsavedChanges());
    }

    void annotationDialogCancelAndProtection()
    {
        QTemporaryDir output;QVERIFY(output.isValid());
        MainWindow window(true);window.show();
        auto *capture=window.findChild<QPushButton*>("captureButton");
        auto *canvas=window.findChild<ImageCanvas*>();
        auto *directory=window.findChild<QLineEdit*>("outputDirectory");directory->setText(output.path());
        window.findChild<QCheckBox*>("autoSave")->setChecked(false);
        QTRY_VERIFY_WITH_TIMEOUT(capture->isEnabled(),5000);capture->click();
        QTRY_VERIFY_WITH_TIMEOUT(!canvas->sourceImage().isNull()&&canvas->isEnabled(),5000);
        auto *add=window.findChild<QPushButton*>("addTextButton");add->click();
        const QPoint point=canvas->imageToWidget({300,180}).toPoint();
        QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QDialog*>("annotationDialog");QVERIFY(dialog);dialog->findChild<QPlainTextEdit*>()->setPlainText(QStringLiteral("取消这段"));dialog->reject();});
        QTest::mouseClick(canvas,Qt::LeftButton,Qt::NoModifier,point);
        QVERIFY(canvas->annotations().isEmpty());QVERIFY(!canvas->hasUnsavedChanges());
        QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QDialog*>("annotationDialog");QVERIFY(dialog);dialog->findChild<QPlainTextEdit*>()->setPlainText(QStringLiteral("启动峰值"));dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();});
        QTest::mouseClick(canvas,Qt::LeftButton,Qt::NoModifier,point);
        QCOMPARE(canvas->annotations().size(),1);QVERIFY(!add->isChecked());
        const auto before=canvas->renderedImage();
        QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QDialog*>("annotationDialog");QVERIFY(dialog);QCOMPARE(dialog->findChild<QPlainTextEdit*>()->toPlainText(),QStringLiteral("启动峰值"));dialog->findChild<QPlainTextEdit*>()->setPlainText(QStringLiteral("取消修改"));dialog->reject();});
        QTest::mouseDClick(canvas,Qt::LeftButton,Qt::NoModifier,canvas->imageToWidget(canvas->annotationBounds(0).center()).toPoint());
        QCOMPARE(canvas->renderedImage(),before);
        QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QFileDialog*>();QVERIFY(dialog);dialog->reject();});
        window.findChild<QPushButton*>("saveButton")->click();QVERIFY(canvas->hasUnsavedChanges());
        auto answer=[&](QMessageBox::StandardButton button){QTimer::singleShot(0,&window,[&,button]{auto *box=window.findChild<QMessageBox*>("unsavedAnnotationsDialog");QVERIFY(box);box->button(button)->click();});};
        answer(QMessageBox::Cancel);capture->click();QCOMPARE(canvas->renderedImage(),before);QVERIFY(capture->isEnabled());
        answer(QMessageBox::Cancel);QVERIFY(!window.close());QVERIFY(window.centralWidget()->isEnabled());
        QFile blocker(output.filePath("blocker"));QVERIFY(blocker.open(QIODevice::WriteOnly));blocker.write("fixture");blocker.close();
        directory->setText(blocker.fileName());
        answer(QMessageBox::Save);capture->click();QVERIFY(canvas->hasUnsavedChanges());QCOMPARE(canvas->renderedImage(),before);QVERIFY(capture->isEnabled());
        directory->setText(output.path());answer(QMessageBox::Discard);capture->click();
        QTRY_VERIFY_WITH_TIMEOUT(canvas->isEnabled(),5000);QVERIFY(canvas->annotations().isEmpty());
    }

    void annotationCaptureFailureAndDelayedClose()
    {
        FakeScpiServer server;
        MainWindow window(true);window.show();
        auto *capture=window.findChild<QPushButton*>("captureButton");
        auto *connection=window.findChild<QPushButton*>("connectButton");
        auto *canvas=window.findChild<ImageCanvas*>();
        auto *mode=window.findChild<QComboBox*>("connectionMode");
        QVERIFY(mode);
        window.findChild<QCheckBox*>("autoSave")->setChecked(false);
        QTRY_VERIFY_WITH_TIMEOUT(capture->isEnabled(),5000);connection->click();
        QTRY_VERIFY_WITH_TIMEOUT(mode->isEnabled(),5000);
        mode->setCurrentIndex(1);window.findChild<QLineEdit*>("hostAddress")->setText("127.0.0.1");
        window.findChild<QSpinBox*>("networkPort")->setValue(server.port());connection->click();
        QTRY_VERIFY_WITH_TIMEOUT(capture->isEnabled(),5000);capture->click();
        QTRY_VERIFY_WITH_TIMEOUT(!canvas->sourceImage().isNull()&&canvas->isEnabled(),5000);
        QVERIFY(canvas->addText(QStringLiteral("异常前"),{30,35},22,Qt::yellow));
        const auto before=canvas->renderedImage();server.setMode(FakeScpiServer::TruncatedImage);
        QTimer::singleShot(0,&window,[&]{auto *box=window.findChild<QMessageBox*>("unsavedAnnotationsDialog");QVERIFY(box);box->button(QMessageBox::Discard)->click();});
        capture->click();QVERIFY(!canvas->isEnabled());QVERIFY(!window.close());QVERIFY(!window.centralWidget()->isEnabled());
        bool cancelled=false;QTimer answer;answer.setInterval(10);
        connect(&answer,&QTimer::timeout,&window,[&]{
            if(auto *box=window.findChild<QMessageBox*>("unsavedAnnotationsDialog")){cancelled=true;box->button(QMessageBox::Cancel)->click();answer.stop();}
        });answer.start();
        QTRY_VERIFY_WITH_TIMEOUT(cancelled,5000);
        QVERIFY(window.centralWidget()->isEnabled());QVERIFY(window.isVisible());
        QCOMPARE(canvas->renderedImage(),before);QVERIFY(canvas->hasUnsavedChanges());
        QVERIFY(window.findChild<QPushButton*>("saveAnnotationsButton")->isEnabled());
        mode->setCurrentIndex(2);connection->click();
        QTRY_VERIFY_WITH_TIMEOUT(capture->isEnabled(),5000);QVERIFY(window.isVisible());
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
