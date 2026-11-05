#include "scopeworker.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QFont>
#include <QLibrary>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTcpSocket>
#include <QtEndian>
#include <cmath>
#include <limits>

namespace {
constexpr qsizetype MaxImageBytes = 32 * 1024 * 1024;
constexpr int OperationTimeoutMs = 8000;
constexpr qint32 VisaMaxCount = 0x3FFF0006;
constexpr quint32 VisaResourceNotFound = 0xBFFF0011;
constexpr quint32 VisaTimeoutAttribute = 0x3FFF001A;
constexpr quint32 VisaTerminationAttribute = 0x3FFF0038;
const QByteArray PngSignature = QByteArray::fromHex("89504e470d0a1a0a");

struct Failure { QString message; };
[[noreturn]] void fail(const QString &message) { throw Failure{message}; }

void checkVisa(qint32 status, const QString &action)
{
    if (status < 0)
        fail(QStringLiteral("%1失败（VISA 0x%2）。请检查 USB 连接，确认仪器没有被其他软件占用。")
                 .arg(action, QString::number(quint32(status), 16).rightJustified(8, QLatin1Char('0')).toUpper()));
}

quint32 pngCrc(const char *data, qsizetype count)
{
    quint32 crc = 0xFFFFFFFFu;
    for (qsizetype i = 0; i < count; ++i) {
        crc ^= static_cast<unsigned char>(data[i]);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

quint32 read32(const char *data)
{
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(data));
}

bool onlyLineEndings(const QByteArray &data)
{
    for (char byte : data)
        if (byte != '\r' && byte != '\n') return false;
    return true;
}

// Incremental framing, so TCP chunk boundaries and PNG chunk boundaries need not match.
struct PngFrame {
    qsizetype position = 8;
    qsizetype completeSize = 0;
    bool sawHeader = false;
    bool sawData = false;

    bool feed(const QByteArray &bytes)
    {
        if (bytes.size() > MaxImageBytes) fail(QStringLiteral("截图数据超过 32 MB，已停止读取。"));
        if (bytes.size() < 8) return false;
        if (!bytes.startsWith(PngSignature)) fail(QStringLiteral("仪器返回的不是 PNG 图片，请检查固件和 SCPI 仿真模式。"));
        if (completeSize) {
            if (!onlyLineEndings(bytes.mid(completeSize))) fail(QStringLiteral("PNG 图片后出现异常数据，已断开连接。"));
            return true;
        }
        while (bytes.size() - position >= 8) {
            const quint32 size = read32(bytes.constData() + position);
            if (size > quint32(MaxImageBytes - 12) || position > MaxImageBytes - 12 - qsizetype(size))
                fail(QStringLiteral("PNG 数据块长度无效。"));
            const qsizetype end = position + 12 + qsizetype(size);
            if (end > bytes.size()) return false;
            const QByteArray kind = bytes.mid(position + 4, 4);
            if (pngCrc(bytes.constData() + position + 4, qsizetype(size) + 4) != read32(bytes.constData() + end - 4))
                fail(QStringLiteral("PNG 校验失败，图片可能未完整传输。"));
            if (!sawHeader) {
                if (kind != "IHDR" || size != 13) fail(QStringLiteral("PNG 图片头无效。"));
                const quint32 width = read32(bytes.constData() + position + 8);
                const quint32 height = read32(bytes.constData() + position + 12);
                if (!width || !height || width > 8192 || height > 8192 || quint64(width) * height > 16 * 1024 * 1024)
                    fail(QStringLiteral("PNG 图片尺寸超出截图范围。"));
                sawHeader = true;
            } else if (kind == "IHDR") {
                fail(QStringLiteral("PNG 含重复图片头。"));
            }
            if (kind == "IDAT") sawData = true;
            position = end;
            if (kind == "IEND") {
                if (size != 0 || !sawData) fail(QStringLiteral("PNG 图片结尾无效。"));
                completeSize = end;
                if (!onlyLineEndings(bytes.mid(end))) fail(QStringLiteral("PNG 图片后出现异常数据，已断开连接。"));
                return true;
            }
        }
        return false;
    }
};

bool isSiglentResource(const QString &resource)
{
    const auto fields = resource.split(QStringLiteral("::"));
    bool okay = false;
    const uint vendor = fields.value(1).toUInt(&okay, 0);
    return resource.startsWith(QStringLiteral("USB"), Qt::CaseInsensitive)
        && resource.endsWith(QStringLiteral("::INSTR"), Qt::CaseInsensitive)
        && okay && vendor == 0xF4EC;
}
}

#ifdef Q_OS_WIN
#define SCOPE_VISA_CALL __stdcall
#else
#define SCOPE_VISA_CALL
#endif

struct ScopeWorker::VisaApi {
    qint32 (SCOPE_VISA_CALL *openManager)(quint32 *) = nullptr;
    qint32 (SCOPE_VISA_CALL *find)(quint32, const char *, quint32 *, quint32 *, char *) = nullptr;
    qint32 (SCOPE_VISA_CALL *findNext)(quint32, char *) = nullptr;
    qint32 (SCOPE_VISA_CALL *open)(quint32, const char *, quint32, quint32, quint32 *) = nullptr;
    qint32 (SCOPE_VISA_CALL *attribute)(quint32, quint32, quintptr) = nullptr;
    qint32 (SCOPE_VISA_CALL *write)(quint32, const char *, quint32, quint32 *) = nullptr;
    qint32 (SCOPE_VISA_CALL *read)(quint32, char *, quint32, quint32 *) = nullptr;
    qint32 (SCOPE_VISA_CALL *close)(quint32) = nullptr;
};

ScopeWorker::ScopeWorker(QObject *parent) : QObject(parent) {}

ScopeWorker::~ScopeWorker()
{
    resetConnection();
    if (m_visa && m_manager) m_visa->close(m_manager);
    delete m_visa;
}

void ScopeWorker::ensureVisa()
{
    if (m_visa && m_manager) return;
    if (!m_library) {
        m_library = new QLibrary(this);
#ifdef Q_OS_WIN
        const QString dll = sizeof(void *) == 8 ? QStringLiteral("visa64.dll") : QStringLiteral("visa32.dll");
        m_library->setFileName(qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows")) + QStringLiteral("/System32/") + dll);
#else
        fail(QStringLiteral("此版本的 USB 功能面向 Windows；可使用网口连接。"));
#endif
    }
    if (!m_library->isLoaded() && !m_library->load())
        fail(QStringLiteral("未找到可用的 VISA 驱动。请安装 NI-VISA 后重试，或改用网口连接。"));
    if (!m_visa) {
        auto api = new VisaApi;
        api->openManager = reinterpret_cast<decltype(api->openManager)>(m_library->resolve("viOpenDefaultRM"));
        api->find = reinterpret_cast<decltype(api->find)>(m_library->resolve("viFindRsrc"));
        api->findNext = reinterpret_cast<decltype(api->findNext)>(m_library->resolve("viFindNext"));
        api->open = reinterpret_cast<decltype(api->open)>(m_library->resolve("viOpen"));
        api->attribute = reinterpret_cast<decltype(api->attribute)>(m_library->resolve("viSetAttribute"));
        api->write = reinterpret_cast<decltype(api->write)>(m_library->resolve("viWrite"));
        api->read = reinterpret_cast<decltype(api->read)>(m_library->resolve("viRead"));
        api->close = reinterpret_cast<decltype(api->close)>(m_library->resolve("viClose"));
        if (!api->openManager || !api->find || !api->findNext || !api->open || !api->attribute || !api->write || !api->read || !api->close) {
            delete api;
            fail(QStringLiteral("VISA 驱动接口不完整，请修复 NI-VISA 安装。"));
        }
        m_visa = api;
    }
    checkVisa(m_visa->openManager(&m_manager), QStringLiteral("打开 VISA 驱动"));
}

void ScopeWorker::resetConnection()
{
    if (m_visa && m_session) m_visa->close(m_session);
    m_session = 0;
    if (m_socket) {
        m_socket->abort();
        delete m_socket;
        m_socket = nullptr;
    }
    m_mode = Mode::None;
}

void ScopeWorker::reportFailure(const QString &message)
{
    resetConnection();
    emit error(message);
    emit disconnected();
}

void ScopeWorker::discover()
{
    const auto done = qScopeGuard([this] { emit operationFinished(); });
    try {
        ensureVisa();
        quint32 finder = 0, count = 0;
        char descriptor[1024] = {};
        const qint32 status = m_visa->find(m_manager, "USB?*::INSTR", &finder, &count, descriptor);
        if (quint32(status) == VisaResourceNotFound) {
            emit devicesFound({});
            return;
        }
        checkVisa(status, QStringLiteral("扫描 USB 仪器"));
        const auto closeFind = qScopeGuard([this, finder] { m_visa->close(finder); });
        QStringList resources;
        for (quint32 index = 0; index < count; ++index) {
            if (index) checkVisa(m_visa->findNext(finder, descriptor), QStringLiteral("枚举 USB 仪器"));
            const QString resource = QString::fromLatin1(descriptor);
            if (isSiglentResource(resource)) resources.append(resource);
        }
        emit devicesFound(resources);
    } catch (const Failure &ex) { reportFailure(ex.message); }
}

QByteArray ScopeWorker::usbQuery(const QByteArray &command, qsizetype limit)
{
    if (!m_visa || !m_session) fail(QStringLiteral("USB 连接已断开，请重新连接。"));
    const QByteArray request = command + '\n';
    quint32 written = 0;
    checkVisa(m_visa->attribute(m_session, VisaTimeoutAttribute, OperationTimeoutMs), QStringLiteral("设置通信超时"));
    checkVisa(m_visa->write(m_session, request.constData(), quint32(request.size()), &written), QStringLiteral("发送截图查询"));
    if (written != quint32(request.size())) fail(QStringLiteral("命令未完整发送，请重新连接。"));
    QElapsedTimer timer;
    timer.start();
    QByteArray result;
    char block[65536];
    while (true) {
        const qint64 remaining = OperationTimeoutMs - timer.elapsed();
        if (remaining <= 0) fail(QStringLiteral("仪器响应超时，请重新连接后重试。"));
        checkVisa(m_visa->attribute(m_session, VisaTimeoutAttribute, quintptr(remaining)), QStringLiteral("设置通信超时"));
        quint32 count = 0;
        const qint32 status = m_visa->read(m_session, block, sizeof(block), &count);
        checkVisa(status, QStringLiteral("读取仪器响应"));
        if (count > sizeof(block) || result.size() + count > limit) fail(QStringLiteral("仪器响应超出预期长度。"));
        result.append(block, count);
        if (status != VisaMaxCount) return result;
        if (!count) fail(QStringLiteral("仪器传输中断，未收到完整响应。"));
    }
}

void ScopeWorker::identify(const QByteArray &reply)
{
    const QList<QByteArray> fields = reply.trimmed().split(',');
    const QString model = QString::fromLatin1(fields.value(1)).trimmed();
    const QRegularExpression supported(QStringLiteral("^SDS3[0-9]{3}X[ -]?HD$"), QRegularExpression::CaseInsensitiveOption);
    if (fields.size() < 2 || !fields[0].toUpper().contains("SIGLENT") || !supported.match(model).hasMatch())
        fail(QStringLiteral("当前设备不是支持的 SDS3000X HD 系列（识别为：%1）。").arg(model.isEmpty() ? QStringLiteral("未知设备") : model));
    const QString firmware = fields.size() > 3 ? QString::fromLatin1(fields[3]).trimmed() : QStringLiteral("未知");
    emit connected(model, firmware, false);
}

void ScopeWorker::openUsb(const QString &resource)
{
    const auto done = qScopeGuard([this] { emit operationFinished(); });
    try {
        resetConnection();
        if (!isSiglentResource(resource.trimmed())) fail(QStringLiteral("请选择扫描到的 SIGLENT USB 仪器。"));
        ensureVisa();
        const QByteArray name = resource.trimmed().toLatin1();
        checkVisa(m_visa->open(m_manager, name.constData(), 0, 3000, &m_session), QStringLiteral("连接 USB 仪器"));
        checkVisa(m_visa->attribute(m_session, VisaTimeoutAttribute, OperationTimeoutMs), QStringLiteral("设置通信超时"));
        checkVisa(m_visa->attribute(m_session, VisaTerminationAttribute, 0), QStringLiteral("设置图片传输"));
        m_mode = Mode::Usb;
        identify(usbQuery("*IDN?", 4096));
    } catch (const Failure &ex) { reportFailure(ex.message); }
}

void ScopeWorker::lanWrite(const QByteArray &command)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        fail(QStringLiteral("网口连接已断开，请重新连接。"));
    if (m_socket->bytesAvailable() && !onlyLineEndings(m_socket->readAll()))
        fail(QStringLiteral("通信缓冲区存在未处理数据，请重新连接。"));
    const QByteArray request = command + '\n';
    if (m_socket->write(request) != request.size()) fail(QStringLiteral("网口命令未完整发送。"));
    QElapsedTimer timer;
    timer.start();
    while (m_socket->bytesToWrite()) {
        const int remaining = OperationTimeoutMs - int(timer.elapsed());
        if (remaining <= 0 || !m_socket->waitForBytesWritten(remaining))
            fail(QStringLiteral("网口发送超时：%1").arg(m_socket->errorString()));
    }
}

QByteArray ScopeWorker::lanIdentity()
{
    lanWrite("*IDN?");
    QElapsedTimer timer;
    timer.start();
    QByteArray reply;
    while (!reply.contains('\n')) {
        if (m_socket->bytesAvailable()) reply += m_socket->readAll();
        if (reply.size() > 4096) fail(QStringLiteral("设备身份响应长度异常。"));
        if (reply.contains('\n')) break;
        const int remaining = OperationTimeoutMs - int(timer.elapsed());
        if (remaining <= 0 || !m_socket->waitForReadyRead(remaining))
            fail(QStringLiteral("未收到设备身份，请确认 IP 地址、端口和网络连接。"));
    }
    const qsizetype lineEnd = reply.indexOf('\n');
    if (!onlyLineEndings(reply.mid(lineEnd + 1))) fail(QStringLiteral("设备身份响应包含异常数据。"));
    return reply.left(lineEnd);
}

void ScopeWorker::openLan(const QString &host, quint16 port)
{
    const auto done = qScopeGuard([this] { emit operationFinished(); });
    try {
        resetConnection();
        const QString target = host.trimmed();
        if (target.isEmpty() || target.contains(QRegularExpression(QStringLiteral("[\\s/\\r\\n]"))) || !port)
            fail(QStringLiteral("请输入有效的示波器 IP 地址或主机名及端口。"));
        m_socket = new QTcpSocket(this);
        m_socket->connectToHost(target, port);
        if (!m_socket->waitForConnected(3000)) fail(QStringLiteral("无法连接示波器：%1").arg(m_socket->errorString()));
        m_mode = Mode::Lan;
        identify(lanIdentity());
    } catch (const Failure &ex) { reportFailure(ex.message); }
}

QByteArray ScopeWorker::lanScreenshot()
{
    lanWrite(":PRINt? PNG");
    QElapsedTimer timer;
    timer.start();
    QByteArray data;
    PngFrame frame;
    while (true) {
        if (m_socket->bytesAvailable()) data += m_socket->readAll();
        if (frame.feed(data)) return data.left(frame.completeSize);
        const int remaining = OperationTimeoutMs - int(timer.elapsed());
        if (remaining <= 0) fail(QStringLiteral("截图读取超时，未收到完整 PNG。"));
        if (!m_socket->waitForReadyRead(remaining))
            fail(QStringLiteral("截图传输中断或超时，未保存不完整图片。"));
    }
}

bool ScopeWorker::decodeScreenshot(const QByteArray &bytes, QImage *image, QString *error)
{
    if (image) *image = QImage();
    if (error) error->clear();
    try {
        PngFrame frame;
        if (!frame.feed(bytes)) fail(QStringLiteral("PNG 图片不完整。"));
        const QImage decoded = QImage::fromData(bytes.left(frame.completeSize), "PNG");
        if (decoded.isNull()) fail(QStringLiteral("PNG 图片无法解码。"));
        if (image) *image = decoded;
        return true;
    } catch (const Failure &ex) {
        if (error) *error = ex.message;
        return false;
    }
}

void ScopeWorker::useDemo()
{
    const auto done = qScopeGuard([this] { emit operationFinished(); });
    resetConnection();
    m_mode = Mode::Demo;
    emit connected(QStringLiteral("SDS3054X HD · 演示"), QStringLiteral("模拟画面"), true);
}

void ScopeWorker::capture()
{
    const auto done = qScopeGuard([this] { emit operationFinished(); });
    try {
        QByteArray bytes;
        if (m_mode == Mode::Demo) bytes = makeDemoPng();
        else if (m_mode == Mode::Usb) bytes = usbQuery(":PRINt? PNG", MaxImageBytes);
        else if (m_mode == Mode::Lan) bytes = lanScreenshot();
        else fail(QStringLiteral("请先连接示波器，或进入演示模式。"));
        QImage image;
        QString problem;
        if (!decodeScreenshot(bytes, &image, &problem)) fail(problem);
        emit captured(image, m_mode == Mode::Demo);
    } catch (const Failure &ex) { reportFailure(ex.message); }
}

void ScopeWorker::disconnectDevice()
{
    const auto done = qScopeGuard([this] { emit operationFinished(); });
    resetConnection();
    emit disconnected();
}

QByteArray ScopeWorker::makeDemoPng()
{
    QImage image(1280, 800, QImage::Format_RGB32);
    image.fill(QColor("#101722"));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(0, 0, 1280, 68, QColor("#192434"));
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 17, QFont::DemiBold));
    painter.setPen(QColor("#edf3fc"));
    painter.drawText(QRect(28, 0, 700, 68), Qt::AlignVCenter, QStringLiteral("SDS3054X HD  /  截图演示"));
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 11));
    painter.setPen(QColor("#9bacbf"));
    painter.drawText(QRect(880, 0, 365, 68), Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("示例信号 · 1.00 kHz"));
    const QRectF grid(44, 96, 1192, 536);
    painter.setPen(QPen(QColor("#253346"), 1));
    for (int i = 0; i <= 12; ++i) {
        const qreal x = grid.left() + grid.width() * i / 12;
        painter.drawLine(QPointF(x, grid.top()), QPointF(x, grid.bottom()));
    }
    for (int i = 0; i <= 8; ++i) {
        const qreal y = grid.top() + grid.height() * i / 8;
        painter.drawLine(QPointF(grid.left(), y), QPointF(grid.right(), y));
    }
    painter.setPen(QPen(QColor("#52637a"), 1, Qt::DashLine));
    painter.drawLine(QPointF(grid.left(), grid.center().y()), QPointF(grid.right(), grid.center().y()));
    painter.drawLine(QPointF(grid.center().x(), grid.top()), QPointF(grid.center().x(), grid.bottom()));
    constexpr double pi = 3.14159265358979323846;
    for (int channel = 0; channel < 2; ++channel) {
        QPainterPath path;
        for (int x = 0; x <= int(grid.width()); ++x) {
            const double phase = double(x) / grid.width() * 6.0 * pi;
            const double y = grid.center().y() + (channel ? 72 : -48) - (channel ? 78 : 145) * std::sin(phase + (channel ? 0.8 : 0.0));
            const QPointF point(grid.left() + x, y);
            if (!x) path.moveTo(point); else path.lineTo(point);
        }
        painter.setPen(QPen(channel ? QColor("#4ad5e8") : QColor("#f5ce5a"), 2.6));
        painter.drawPath(path);
    }
    painter.fillRect(44, 655, 1192, 60, QColor("#192434"));
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 13));
    painter.setPen(QColor("#f5ce5a"));
    painter.drawText(QRect(64, 655, 380, 60), Qt::AlignVCenter, QStringLiteral("CH1   5.00 V/div   Vpp  20.0 V"));
    painter.setPen(QColor("#4ad5e8"));
    painter.drawText(QRect(460, 655, 390, 60), Qt::AlignVCenter, QStringLiteral("CH2   2.00 V/div   Vpp  8.00 V"));
    painter.setPen(QColor("#bac7d6"));
    painter.drawText(QRect(900, 655, 310, 60), Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("M  200 μs/div"));
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 18, QFont::Bold));
    painter.setPen(QColor("#f1b76c"));
    painter.drawText(QRect(0, 730, 1280, 48), Qt::AlignCenter, QStringLiteral("演示 · 非仪器实测"));
    painter.end();
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return png;
}
