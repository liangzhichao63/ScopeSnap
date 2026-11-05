#include "mainwindow.h"
#include <QApplication>
#include <QCheckBox>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QTimer>

int main(int argc,char **argv) {
    QApplication app(argc,argv);
    app.setApplicationName("ScopeSnap");app.setOrganizationName("ScopeSnap");app.setApplicationVersion(APP_VERSION);
    app.setStyle("Fusion");QImageReader::setAllocationLimit(128);
    QFont font(QStringLiteral("Microsoft YaHei UI"),9);app.setFont(font);
    QPixmap icon(64,64);icon.fill(Qt::transparent);
    {QPainter p(&icon);p.setRenderHint(QPainter::Antialiasing);p.setPen(Qt::NoPen);p.setBrush(QColor("#2468db"));p.drawRoundedRect(1,1,62,62,15,15);QPainterPath path;path.moveTo(10,34);path.lineTo(21,34);path.lineTo(28,15);path.lineTo(37,49);path.lineTo(44,28);path.lineTo(54,28);p.setPen(QPen(Qt::white,4,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.drawPath(path);}
    app.setWindowIcon(QIcon(icon));
    QCommandLineParser parser;parser.setApplicationDescription(QStringLiteral("SIGLENT 示波器截图工具"));parser.addHelpOption();parser.addVersionOption();
    parser.addOption({"demo",QStringLiteral("启动离线演示模式")});
    parser.addOption({"snapshot",QStringLiteral("保存应用界面图并退出，仅用于离线验收"),"file"});
    parser.addOption({"size",QStringLiteral("验收窗口尺寸，例如 1260x850"),"size"});
    parser.process(app);
    if(parser.isSet("snapshot")) {
        app.setApplicationName("ScopeSnap-Validation");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,QCoreApplication::applicationDirPath()+"/validation-settings");
    }
    MainWindow window(parser.isSet("demo"));
    if(parser.isSet("size")){const auto parts=parser.value("size").split('x');if(parts.size()==2)window.resize(parts[0].toInt(),parts[1].toInt());}
    window.show();
    if(parser.isSet("snapshot")) {
        const auto path=parser.value("snapshot");
        window.findChild<QCheckBox*>("autoSave")->setChecked(false);
        if(parser.isSet("demo"))QTimer::singleShot(400,&window,[&window]{window.findChild<QPushButton*>("captureButton")->click();});
        QTimer::singleShot(1500,&window,[&window,path,&app]{QDir().mkpath(QFileInfo(path).absolutePath());const bool ok=window.grab().save(path,"PNG");app.exit(ok?0:2);});
    }
    return app.exec();
}
