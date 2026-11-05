#pragma once

#include <QObject>
#include <QByteArray>
#include <QImage>
#include <QStringList>

class QLibrary;
class QTcpSocket;

class ScopeWorker final : public QObject
{
    Q_OBJECT
public:
    explicit ScopeWorker(QObject *parent = nullptr);
    ~ScopeWorker() override;

    static bool decodeScreenshot(const QByteArray &bytes, QImage *image, QString *error);
    static QByteArray makeDemoPng();

public slots:
    void discover();
    void openUsb(const QString &resource);
    void openLan(const QString &host, quint16 port = 5025);
    void useDemo();
    void capture();
    void disconnectDevice();

signals:
    void devicesFound(const QStringList &resources);
    void connected(const QString &model, const QString &firmware, bool demo);
    void captured(const QImage &image, bool demo);
    void error(const QString &message);
    void disconnected();
    void operationFinished();

private:
    enum class Mode { None, Usb, Lan, Demo };
    struct VisaApi;
    void ensureVisa();
    void resetConnection();
    void reportFailure(const QString &message);
    QByteArray usbQuery(const QByteArray &command, qsizetype limit);
    void lanWrite(const QByteArray &command);
    QByteArray lanIdentity();
    QByteArray lanScreenshot();
    void identify(const QByteArray &reply);

    Mode m_mode = Mode::None;
    QLibrary *m_library = nullptr;
    VisaApi *m_visa = nullptr;
    quint32 m_manager = 0;
    quint32 m_session = 0;
    QTcpSocket *m_socket = nullptr;
};
