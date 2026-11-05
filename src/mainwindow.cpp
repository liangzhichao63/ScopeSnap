#include "mainwindow.h"
#include "scopeworker.h"
#include "imagecanvas.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

static QLabel *caption(const QString &text) {
    auto *label = new QLabel(text); label->setObjectName("caption"); return label;
}
static QFrame *line() {
    auto *frame = new QFrame; frame->setFrameShape(QFrame::HLine);frame->setObjectName("separator");return frame;
}

MainWindow::MainWindow(bool startDemo, QWidget *parent) : QMainWindow(parent) {
    setObjectName("mainWindow");
    setWindowTitle(QStringLiteral("ScopeSnap · 示波器截图"));
    resize(1260, 850);setMinimumSize(940, 680);
    buildUi();
    worker_ = new ScopeWorker;
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &ScopeWorker::devicesFound, this, [this](const QStringList &resources) {
        const QString old = devices_->currentData().toString();
        devices_->clear();
        for (const auto &resource : resources) {
            devices_->addItem(resource,resource);
            devices_->setItemData(devices_->count()-1,resource,Qt::ToolTipRole);
        }
        const int index = devices_->findData(old);if(index>=0)devices_->setCurrentIndex(index);
        if(resources.isEmpty()) { devices_->addItem(QStringLiteral("未发现设备"),QString());showMessage(QStringLiteral("请开机，并将背面 USB Device 方口连接到电脑。")); }
        else showMessage(QStringLiteral("发现 %1 台设备，请选择并连接。").arg(resources.size()));
    });
    connect(worker_, &ScopeWorker::connected, this, [this](const QString &model,const QString &firmware,bool demo) {
        connected_=true;demo_=demo;
        deviceInfo_->setText(demo ? QStringLiteral("离线演示\n使用模拟波形，不连接仪器") : QStringLiteral("%1\n固件 %2").arg(model,firmware));
        statusBadge_->setText(demo ? QStringLiteral("● 演示模式") : QStringLiteral("● 已连接"));
        statusBadge_->setStyleSheet(demo ? "color:#a45d06;background:#fff3dd;" : "color:#128059;background:#e5f5ed;");
        showMessage(demo ? QStringLiteral("已进入演示模式。截图会标注为演示图。") : QStringLiteral("设备已连接，可以截图。"));
        updateActions();
    });
    connect(worker_, &ScopeWorker::captured, this, &MainWindow::onImage);
    connect(worker_, &ScopeWorker::disconnected, this, [this] {
        connected_=false;demo_=false;deviceInfo_->setText(QStringLiteral("尚未连接示波器"));
        statusBadge_->setText(QStringLiteral("● 未连接"));statusBadge_->setStyleSheet("color:#738094;background:#edf0f5;");
        updateActions();
    });
    connect(worker_, &ScopeWorker::error, this, [this](const QString &message){showMessage(message,true);});
    connect(worker_, &ScopeWorker::operationFinished, this, [this] {
        setBusy(false);
        if(closeRequested_)QTimer::singleShot(0,this,&QWidget::close);
    });
    thread_.start();
    updateActions();
    if(startDemo) {
        mode_->setCurrentIndex(2);
        QTimer::singleShot(0,this,[this]{toggleConnection();});
    } else QTimer::singleShot(0,this,&MainWindow::scan);
}

MainWindow::~MainWindow() {
    QSettings settings;
    settings.setValue("outputDirectory",directory_->text());settings.setValue("prefix",prefix_->text());
    settings.setValue("autoSave",autoSave_->isChecked());settings.setValue("host",host_->text());settings.setValue("port",port_->value());
    if(thread_.isRunning()) {
        QMetaObject::invokeMethod(worker_,&ScopeWorker::disconnectDevice,Qt::BlockingQueuedConnection);
        thread_.quit();thread_.wait();
    }
}

void MainWindow::buildUi() {
    QSettings settings;
    auto *central = new QWidget;setCentralWidget(central);
    auto *root = new QVBoxLayout(central);root->setContentsMargins(24,20,24,14);root->setSpacing(18);
    auto *header = new QHBoxLayout;
    auto *logo = new QLabel(QStringLiteral("∿"));logo->setAlignment(Qt::AlignCenter);logo->setFixedSize(48,48);logo->setStyleSheet("background:#2468db;color:white;border-radius:12px;font-size:38px;font-weight:600;");
    header->addWidget(logo);header->addSpacing(4);
    auto *titleColumn = new QVBoxLayout;titleColumn->setSpacing(1);
    auto *title = new QLabel("ScopeSnap");title->setStyleSheet("font-size:24px;font-weight:700;color:#1c2c46;");
    auto *subtitle = new QLabel(QStringLiteral("示波器截图  /  SIGLENT SDS3000X HD"));subtitle->setStyleSheet("color:#79879a;font-size:12px;");
    titleColumn->addWidget(title);titleColumn->addWidget(subtitle);header->addLayout(titleColumn);header->addStretch();
    statusBadge_ = new QLabel(QStringLiteral("● 未连接"));statusBadge_->setObjectName("statusBadge");statusBadge_->setStyleSheet("color:#738094;background:#edf0f5;");
    header->addWidget(statusBadge_);
    auto *aboutButton = new QPushButton(QStringLiteral("关于"));aboutButton->setObjectName("quiet");header->addWidget(aboutButton);connect(aboutButton,&QPushButton::clicked,this,&MainWindow::about);
    root->addLayout(header);

    auto *body = new QHBoxLayout;body->setSpacing(18);
    auto *sidebar = new QFrame;sidebar->setObjectName("card");sidebar->setFixedWidth(285);
    auto *left = new QVBoxLayout(sidebar);left->setContentsMargins(18,18,18,18);left->setSpacing(9);
    auto *connectionTitle = new QLabel(QStringLiteral("连接示波器"));connectionTitle->setObjectName("sectionTitle");left->addWidget(connectionTitle);
    left->addWidget(caption(QStringLiteral("连接方式")));
    mode_ = new QComboBox;mode_->setObjectName("connectionMode");mode_->addItems({QStringLiteral("USB 数据线"),QStringLiteral("网络 LAN"),QStringLiteral("离线演示")});left->addWidget(mode_);
    connectionPages_ = new QStackedWidget;
    auto *usbPage = new QWidget;auto *usbLayout = new QVBoxLayout(usbPage);usbLayout->setContentsMargins(0,0,0,0);usbLayout->setSpacing(10);
    usbLayout->addWidget(caption(QStringLiteral("USB 仪器")));
    devices_ = new QComboBox;devices_->setObjectName("deviceList");devices_->setMinimumWidth(0);devices_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);devices_->setMinimumContentsLength(10);
    devices_->addItem(QStringLiteral("点击刷新查找设备"),QString());usbLayout->addWidget(devices_);
    scanButton_ = new QPushButton(QStringLiteral("刷新设备"));scanButton_->setObjectName("scanButton");usbLayout->addWidget(scanButton_);connect(scanButton_,&QPushButton::clicked,this,&MainWindow::scan);
    auto *visaLink = new QLabel(QStringLiteral("USB 需要 VISA · <a href='install-driver'>安装驱动（需联网）</a>"));visaLink->setWordWrap(true);visaLink->setStyleSheet("font-size:11px;color:#8994a5;");usbLayout->addWidget(visaLink);
    connect(visaLink,&QLabel::linkActivated,this,[this] {
        const QString installer=QCoreApplication::applicationDirPath()+"/drivers/ni-visa_26.8_online.exe";
        if(QFileInfo::exists(installer)) {
            if(!QDesktopServices::openUrl(QUrl::fromLocalFile(installer)))showMessage(QStringLiteral("安装器无法启动，请到 drivers 文件夹手动打开。"),true);
        } else QDesktopServices::openUrl(QUrl("https://www.ni.com/en/support/downloads/drivers/download.ni-visa.html"));
    });
    usbLayout->addStretch();connectionPages_->addWidget(usbPage);
    auto *lanPage = new QWidget;auto *lanLayout = new QVBoxLayout(lanPage);lanLayout->setContentsMargins(0,0,0,0);lanLayout->setSpacing(10);
    lanLayout->addWidget(caption(QStringLiteral("示波器 IP 地址")));
    host_ = new QLineEdit(settings.value("host").toString());host_->setObjectName("hostAddress");host_->setPlaceholderText("192.168.1.100");lanLayout->addWidget(host_);
    auto *portRow = new QHBoxLayout;portRow->addWidget(caption(QStringLiteral("端口")));port_ = new QSpinBox;port_->setRange(1,65535);port_->setValue(settings.value("port",5025).toInt());port_->setObjectName("networkPort");portRow->addWidget(port_);lanLayout->addLayout(portRow);
    auto *lanHint = new QLabel(QStringLiteral("电脑与示波器接入同一局域网。网络连接无需安装 VISA。"));lanHint->setWordWrap(true);lanHint->setObjectName("hint");lanLayout->addWidget(lanHint);lanLayout->addStretch();connectionPages_->addWidget(lanPage);
    auto *demoPage = new QWidget;auto *demoLayout = new QVBoxLayout(demoPage);demoLayout->setContentsMargins(0,5,0,0);
    auto *demoHint = new QLabel(QStringLiteral("无需连接设备\n\n使用带有明显水印的演示波形，体验预览、保存和复制。"));demoHint->setWordWrap(true);demoHint->setStyleSheet("color:#8a6529;background:#fff8e9;padding:12px;border-radius:8px;");demoLayout->addWidget(demoHint);demoLayout->addStretch();connectionPages_->addWidget(demoPage);
    connectionPages_->setFixedHeight(181);left->addWidget(connectionPages_);
    connect(mode_,&QComboBox::currentIndexChanged,connectionPages_,&QStackedWidget::setCurrentIndex);
    connect(mode_,&QComboBox::currentIndexChanged,this,[this]{updateActions();});
    connect(devices_,&QComboBox::currentIndexChanged,this,[this]{updateActions();});
    connect(host_,&QLineEdit::textChanged,this,[this]{updateActions();});
    connectButton_ = new QPushButton(QStringLiteral("连接设备"));connectButton_->setObjectName("connectButton");left->addWidget(connectButton_);connect(connectButton_,&QPushButton::clicked,this,&MainWindow::toggleConnection);
    deviceInfo_ = new QLabel(QStringLiteral("尚未连接示波器"));deviceInfo_->setWordWrap(true);deviceInfo_->setObjectName("deviceInfo");left->addWidget(deviceInfo_);
    left->addSpacing(2);left->addWidget(line());left->addSpacing(2);
    auto *saveTitle = new QLabel(QStringLiteral("保存设置"));saveTitle->setObjectName("sectionTitle");left->addWidget(saveTitle);
    left->addWidget(caption(QStringLiteral("保存位置")));
    const QString pictures = QDir::homePath()+"/Pictures/示波器截图";
    directory_ = new QLineEdit(settings.value("outputDirectory",pictures).toString());directory_->setObjectName("outputDirectory");left->addWidget(directory_);
    auto *directoryButtons = new QHBoxLayout;auto *browse = new QPushButton(QStringLiteral("选择文件夹"));auto *open = new QPushButton(QStringLiteral("打开目录"));directoryButtons->addWidget(browse);directoryButtons->addWidget(open);left->addLayout(directoryButtons);
    connect(browse,&QPushButton::clicked,this,[this]{const auto dir=QFileDialog::getExistingDirectory(this,QStringLiteral("选择截图保存位置"),outputDirectory());if(!dir.isEmpty())directory_->setText(QDir::toNativeSeparators(dir));});
    connect(open,&QPushButton::clicked,this,[this]{const auto dir=outputDirectory();if(!QDir(dir).exists()){showMessage(QStringLiteral("目录尚未创建，首次保存截图时会自动创建。"));return;}QDesktopServices::openUrl(QUrl::fromLocalFile(dir));});
    left->addWidget(caption(QStringLiteral("文件名前缀 / 工况")));
    prefix_ = new QLineEdit(settings.value("prefix","SDS3054X_HD").toString());prefix_->setObjectName("filenamePrefix");prefix_->setPlaceholderText(QStringLiteral("例如：电机启动_24V"));left->addWidget(prefix_);
    autoSave_ = new QCheckBox(QStringLiteral("截图后自动保存原图"));autoSave_->setChecked(settings.value("autoSave",true).toBool());autoSave_->setObjectName("autoSave");left->addWidget(autoSave_);
    auto *nameHint = new QLabel(QStringLiteral("文件名自动附加时间，保留每次截图。"));nameHint->setObjectName("hint");nameHint->setWordWrap(true);left->addWidget(nameHint);left->addStretch();
    auto *sidebarScroll = new QScrollArea;sidebarScroll->setWidget(sidebar);sidebarScroll->setWidgetResizable(true);sidebarScroll->setFixedWidth(298);sidebarScroll->setFrameShape(QFrame::NoFrame);sidebarScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);body->addWidget(sidebarScroll);

    auto *right = new QVBoxLayout;right->setSpacing(12);
    auto *previewCard = new QFrame;previewCard->setObjectName("card");auto *previewLayout = new QVBoxLayout(previewCard);previewLayout->setContentsMargins(0,0,0,0);previewLayout->setSpacing(0);
    auto *previewBar = new QHBoxLayout;previewBar->setContentsMargins(18,12,14,12);auto *previewTitle = new QLabel(QStringLiteral("屏幕预览"));previewTitle->setObjectName("sectionTitle");previewBar->addWidget(previewTitle);previewBar->addStretch();
    auto *fit = new QPushButton(QStringLiteral("适应窗口"));auto *actual = new QPushButton("100%");fit->setObjectName("fitImageButton");actual->setObjectName("actualImageButton");previewBar->addWidget(fit);previewBar->addWidget(actual);previewLayout->addLayout(previewBar);
    auto *annotationBar = new QHBoxLayout;annotationBar->setSpacing(6);annotationBar->setContentsMargins(14,0,14,10);
    addTextButton_=new QPushButton(QStringLiteral("＋ 文字"));addTextButton_->setObjectName("addTextButton");addTextButton_->setCheckable(true);annotationBar->addWidget(addTextButton_);
    editTextButton_=new QPushButton(QStringLiteral("编辑"));editTextButton_->setObjectName("editTextButton");annotationBar->addWidget(editTextButton_);
    deleteTextButton_=new QPushButton(QStringLiteral("删除"));deleteTextButton_->setObjectName("deleteTextButton");annotationBar->addWidget(deleteTextButton_);
    undoTextButton_=new QPushButton(QStringLiteral("撤销"));undoTextButton_->setObjectName("undoTextButton");annotationBar->addWidget(undoTextButton_);
    clearTextButton_=new QPushButton(QStringLiteral("清空"));clearTextButton_->setObjectName("clearTextButton");annotationBar->addWidget(clearTextButton_);
    annotationBar->addStretch();previewLayout->addLayout(annotationBar);
    auto *scroll = new QScrollArea;scroll->setObjectName("previewScrollArea");scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);canvas_=new ImageCanvas;scroll->setWidget(canvas_);previewLayout->addWidget(scroll,1);
    connect(fit,&QPushButton::clicked,this,[this]{canvas_->setActual(false);});connect(actual,&QPushButton::clicked,this,[this]{canvas_->setActual(true);});
    connect(addTextButton_,&QPushButton::toggled,this,[this](bool enabled){canvas_->setAddMode(enabled);updateAnnotationActions();});
    connect(canvas_,&ImageCanvas::addRequested,this,[this](const QPointF &position){editAnnotation(position);});
    connect(canvas_,&ImageCanvas::editRequested,this,[this](int index){editAnnotation({},index);});
    connect(editTextButton_,&QPushButton::clicked,this,[this]{editAnnotation({},canvas_->selectedIndex());});
    connect(deleteTextButton_,&QPushButton::clicked,canvas_,&ImageCanvas::deleteSelected);
    connect(undoTextButton_,&QPushButton::clicked,canvas_,&ImageCanvas::undo);
    connect(clearTextButton_,&QPushButton::clicked,canvas_,&ImageCanvas::clearAnnotations);
    connect(canvas_,&ImageCanvas::annotationsChanged,this,&MainWindow::updateAnnotationActions);
    connect(canvas_,&ImageCanvas::selectionChanged,this,&MainWindow::updateAnnotationActions);
    auto *imageFooter = new QHBoxLayout;imageFooter->setContentsMargins(18,10,18,10);
    imageInfo_ = new QLabel(QStringLiteral("尚无截图"));imageInfo_->setObjectName("hint");imageFooter->addWidget(imageInfo_);imageFooter->addStretch();previewLayout->addLayout(imageFooter);right->addWidget(previewCard,1);
    annotationStatus_=new QLabel(QStringLiteral("截图后可点击「＋ 文字」添加标注"));annotationStatus_->setObjectName("annotationStatus");annotationStatus_->setWordWrap(true);right->addWidget(annotationStatus_);
    auto *actions = new QHBoxLayout;actions->setSpacing(10);
    captureButton_=new QPushButton(QStringLiteral("截图  F5"));captureButton_->setObjectName("captureButton");captureButton_->setMinimumHeight(43);actions->addWidget(captureButton_,2);
    saveAnnotationsButton_=new QPushButton(QStringLiteral("保存标注"));saveAnnotationsButton_->setObjectName("saveAnnotationsButton");saveAnnotationsButton_->setMinimumHeight(43);actions->addWidget(saveAnnotationsButton_,1);
    saveButton_=new QPushButton(QStringLiteral("另存为…"));saveButton_->setObjectName("saveButton");saveButton_->setMinimumHeight(43);actions->addWidget(saveButton_,1);
    copyButton_=new QPushButton(QStringLiteral("复制图片"));copyButton_->setObjectName("copyButton");copyButton_->setMinimumHeight(43);actions->addWidget(copyButton_,1);right->addLayout(actions);
    connect(captureButton_,&QPushButton::clicked,this,&MainWindow::capture);connect(saveButton_,&QPushButton::clicked,this,[this]{saveImage(true);});connect(copyButton_,&QPushButton::clicked,this,&MainWindow::copyImage);
    connect(saveAnnotationsButton_,&QPushButton::clicked,this,[this]{saveImage(false);});
    auto *recentLabel = new QLabel(QStringLiteral("本次保存"));recentLabel->setObjectName("caption");right->addWidget(recentLabel);
    history_ = new QListWidget;history_->setObjectName("historyList");history_->setFixedHeight(77);history_->setIconSize(QSize(78,48));history_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);history_->setToolTip(QStringLiteral("双击打开保存的图片"));right->addWidget(history_);
    connect(history_,&QListWidget::itemDoubleClicked,this,[](QListWidgetItem *item){QDesktopServices::openUrl(QUrl::fromLocalFile(item->data(Qt::UserRole).toString()));});
    body->addLayout(right,1);root->addLayout(body,1);
    message_=new QLabel(QStringLiteral("准备就绪"));message_->setObjectName("messageLabel");message_->setWordWrap(true);message_->setMinimumHeight(28);message_->setTextInteractionFlags(Qt::TextSelectableByMouse);root->addWidget(message_);
    auto *captureKey = new QShortcut(QKeySequence(Qt::Key_F5),this);connect(captureKey,&QShortcut::activated,this,&MainWindow::capture);
    auto *copyKey = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),this);connect(copyKey,&QShortcut::activated,this,&MainWindow::copyImage);copyButton_->setToolTip(QStringLiteral("复制当前截图  Ctrl+Shift+C"));
    setStyleSheet(R"(
        QMainWindow { background:#f5f7fb; }
        QWidget { font-family:'Microsoft YaHei UI','Segoe UI'; font-size:12px; color:#344258; }
        QFrame#card { background:white; border:1px solid #dfe5ed; border-radius:12px; }
        QLabel#sectionTitle { font-size:14px; font-weight:600; color:#23334c; }
        QLabel#caption { color:#7a8798; font-size:11px; }
        QLabel#hint { color:#8b96a6; font-size:11px; }
        QLabel#deviceInfo { color:#6b7c91; font-size:11px; min-height:28px; }
        QLabel#statusBadge { padding:8px 13px; border-radius:14px; font-size:12px; }
        QFrame#separator { border:none; background:#e8edf4; max-height:1px; }
        QPushButton { background:white;border:1px solid #d7e0ec;border-radius:7px;padding:8px 12px;font-weight:500; }
        QPushButton:hover { border-color:#8ab0ec;background:#f2f7ff; }
        QPushButton:pressed { background:#e5efff; }
        QPushButton:checked { background:#e4efff;color:#155cc8;border-color:#73a3ed; }
        QPushButton:disabled { color:#aab4c2;background:#f4f6f9;border-color:#e5eaf0; }
        QPushButton#captureButton { background:#2468db;color:white;border-color:#2468db;font-size:14px;font-weight:600; }
        QPushButton#captureButton:hover { background:#1a59c2; }
        QPushButton#captureButton:disabled { background:#bdcde7;border-color:#bdcde7;color:#f7f9fc; }
        QPushButton#connectButton { color:#2468db;background:#eff5ff;border-color:#d2e3ff;font-weight:600; }
        QPushButton#connectButton:disabled { color:#aab4c2;background:#f4f6f9;border-color:#e5eaf0; }
        QPushButton#quiet { border:none;background:transparent;color:#687e9b;padding:6px 10px; }
        QPushButton#quiet:hover { background:#edf3fc; }
        QLineEdit,QComboBox,QSpinBox,QPlainTextEdit { background:#fff;border:1px solid #d9e1eb;border-radius:6px;padding:7px 8px;min-height:18px;selection-background-color:#2468db; }
        QLineEdit:focus,QComboBox:focus,QSpinBox:focus { border-color:#5d94ed; }
        QLineEdit:disabled,QComboBox:disabled,QSpinBox:disabled { background:#f7f8fa;color:#9ca8b7; }
        QComboBox::drop-down { border:none;width:24px; }
        QComboBox::down-arrow { image:url(:/icons/down.xpm);width:12px;height:8px; }
        QComboBox QAbstractItemView { background:white;border:1px solid #d9e1eb;selection-background-color:#e8f0ff;selection-color:#2459a6;outline:none; }
        QCheckBox { spacing:7px; }
        QCheckBox::indicator { width:14px;height:14px; }
        QListWidget { background:white;border:1px solid #dfe5ed;border-radius:8px;padding:4px;outline:none; }
        QListWidget::item { padding:4px;border-radius:4px; }
        QListWidget::item:selected { background:#eaf2ff;color:#2459a6; }
        QScrollBar:vertical { background:#f1f4f8;width:9px;border:none; }
        QScrollBar::handle:vertical { background:#c6d1df;border-radius:4px;min-height:24px; }
        QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }
    )");
}

void MainWindow::scan() {
    if(busy_ || connected_)return;
    setBusy(true,QStringLiteral("正在查找 USB 示波器…"));
    QMetaObject::invokeMethod(worker_,&ScopeWorker::discover,Qt::QueuedConnection);
}
void MainWindow::toggleConnection() {
    if(busy_)return;
    if(connected_) {
        setBusy(true,QStringLiteral("正在断开连接…"));
        QMetaObject::invokeMethod(worker_,&ScopeWorker::disconnectDevice,Qt::QueuedConnection);return;
    }
    const int mode=mode_->currentIndex();
    if(mode==0) {
        auto resource=devices_->currentData().toString();if(resource.isEmpty())return;
        setBusy(true,QStringLiteral("正在连接 USB 示波器…"));
        QMetaObject::invokeMethod(worker_,[this,resource]{worker_->openUsb(resource);},Qt::QueuedConnection);
    } else if(mode==1) {
        auto host=host_->text().trimmed();const quint16 port=quint16(port_->value());if(host.isEmpty())return;
        setBusy(true,QStringLiteral("正在连接 %1…").arg(host));
        QMetaObject::invokeMethod(worker_,[this,host,port]{worker_->openLan(host,port);},Qt::QueuedConnection);
    } else {
        setBusy(true,QStringLiteral("正在准备离线演示…"));
        QMetaObject::invokeMethod(worker_,&ScopeWorker::useDemo,Qt::QueuedConnection);
    }
}
void MainWindow::capture() {
    if(busy_ || !connected_)return;
    if(!confirmDiscardAnnotations())return;
    setBusy(true,demo_ ? QStringLiteral("正在生成演示图…") : QStringLiteral("正在读取示波器屏幕…"));
    QMetaObject::invokeMethod(worker_,&ScopeWorker::capture,Qt::QueuedConnection);
}
void MainWindow::setBusy(bool busy,const QString &message) {
    busy_=busy;updateActions();if(!message.isEmpty())showMessage(message);
}
void MainWindow::updateActions() {
    if(!connectButton_)return;
    mode_->setEnabled(!busy_&&!connected_);devices_->setEnabled(!busy_&&!connected_);host_->setEnabled(!busy_&&!connected_);port_->setEnabled(!busy_&&!connected_);
    scanButton_->setEnabled(!busy_&&!connected_);
    const bool ready=mode_->currentIndex()==2 || (mode_->currentIndex()==1&&!host_->text().trimmed().isEmpty()) || (mode_->currentIndex()==0&&!devices_->currentData().toString().isEmpty());
    connectButton_->setEnabled(!busy_&&(connected_||ready));connectButton_->setText(connected_?QStringLiteral("断开连接"):(mode_->currentIndex()==2?QStringLiteral("开始演示"):QStringLiteral("连接设备")));
    captureButton_->setEnabled(!busy_&&connected_);captureButton_->setText(busy_ ? QStringLiteral("处理中…") : QStringLiteral("截图  F5"));
    saveButton_->setEnabled(!busy_&&!image_.isNull());copyButton_->setEnabled(!image_.isNull());
    updateAnnotationActions();
}
void MainWindow::showMessage(const QString &message,bool error) {
    message_->setText(message);message_->setStyleSheet(error?"color:#b5433f;":"color:#6d7d92;");
}
void MainWindow::onImage(const QImage &image,bool demo) {
    image_=image;imageDemo_=demo;canvas_->setImage(image);
    imageInfo_->setText(QStringLiteral("%1 × %2 px  ·  %3%4").arg(image.width()).arg(image.height()).arg(QDateTime::currentDateTime().toString("HH:mm:ss"),demo?QStringLiteral("  ·  演示图，非实测数据"):QString()));
    if(autoSave_->isChecked())saveImage(false);else showMessage(QStringLiteral("截图已读取，可以保存或复制。"));
    updateActions();
}
QString MainWindow::outputDirectory() const {
    auto path=directory_->text().trimmed();return path.isEmpty()?QDir::homePath()+"/Pictures/示波器截图":QDir::cleanPath(path);
}
QString MainWindow::suggestedFilename() const {
    QString prefix=prefix_->text().trimmed();prefix.replace(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1f]"),"_");
    prefix=prefix.left(70);while(prefix.endsWith('.')||prefix.endsWith(' '))prefix.chop(1);
    if(prefix.isEmpty())prefix="SDS3054X_HD";
    return (imageDemo_?QStringLiteral("演示_"):QString())+prefix+(canvas_->annotations().isEmpty()?QStringLiteral("_"):QStringLiteral("_标注_"))+QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz")+"_"+QUuid::createUuid().toString(QUuid::Id128).left(4)+".png";
}
bool MainWindow::saveImage(bool choosePath) {
    if(image_.isNull())return false;
    const QImage exported = canvas_->renderedImage();
    QString path=QDir(outputDirectory()).filePath(suggestedFilename());
    if(choosePath) {
        path=QFileDialog::getSaveFileName(this,QStringLiteral("保存示波器截图"),path,QStringLiteral("PNG 图片 (*.png)"));
        if(path.isEmpty())return false;if(!path.endsWith(".png",Qt::CaseInsensitive))path+=".png";
    }
    if(!QDir().mkpath(QFileInfo(path).absolutePath())){showMessage(QStringLiteral("无法创建保存目录，请选择有写入权限的文件夹。"),true);return false;}
    QSaveFile file(path);
    if(!file.open(QIODevice::WriteOnly)||!exported.save(&file,"PNG")||!file.commit()) {showMessage(QStringLiteral("图片保存失败：%1").arg(file.errorString()),true);return false;}
    auto *item=new QListWidgetItem(QIcon(QPixmap::fromImage(exported.scaled(78,48,Qt::KeepAspectRatio,Qt::SmoothTransformation))),QFileInfo(path).fileName());
    item->setData(Qt::UserRole,path);item->setToolTip(path);history_->insertItem(0,item);while(history_->count()>100)delete history_->takeItem(history_->count()-1);
    showMessage(QStringLiteral("已保存：%1").arg(QDir::toNativeSeparators(path)));
    canvas_->markSaved();return true;
}
void MainWindow::copyImage() {
    if(image_.isNull())return;QApplication::clipboard()->setImage(canvas_->renderedImage());showMessage(QStringLiteral("图片及标注已复制，可以粘贴到微信、Word 或报告中。"));
}

void MainWindow::updateAnnotationActions() {
    if(!annotationStatus_)return;
    const bool available=!busy_&&!image_.isNull();
    canvas_->setEnabled(!busy_);
    addTextButton_->setEnabled(available);
    addTextButton_->setChecked(canvas_->addMode());
    editTextButton_->setEnabled(available&&canvas_->selectedIndex()>=0);
    deleteTextButton_->setEnabled(available&&canvas_->selectedIndex()>=0);
    undoTextButton_->setEnabled(available&&canvas_->canUndo());
    clearTextButton_->setEnabled(available&&!canvas_->annotations().isEmpty());
    saveAnnotationsButton_->setEnabled(available&&(!canvas_->annotations().isEmpty()||canvas_->hasUnsavedChanges()));
    if(image_.isNull())annotationStatus_->setText(QStringLiteral("截图后可点击「＋ 文字」添加标注"));
    else if(canvas_->addMode())annotationStatus_->setText(QStringLiteral("点击图片选择文字位置 · Esc 取消"));
    else if(canvas_->hasUnsavedChanges())annotationStatus_->setText(QStringLiteral("标注未保存 · 拖动移动，双击编辑；点击「保存标注」另存新图"));
    else if(!canvas_->annotations().isEmpty())annotationStatus_->setText(QStringLiteral("标注已保存 · 拖动移动，双击编辑，Delete 删除，Ctrl+Z 撤销"));
    else annotationStatus_->setText(QStringLiteral("点击「＋ 文字」后在图上放置 · 保存和复制会包含文字标注"));
    annotationStatus_->setStyleSheet(canvas_->hasUnsavedChanges()?"color:#ab650d;font-size:11px;":"color:#78879b;font-size:11px;");
}

void MainWindow::editAnnotation(const QPointF &position,int index) {
    if(busy_||image_.isNull()||index>=canvas_->annotations().size())return;
    const bool editing=index>=0;
    QDialog dialog(this);dialog.setObjectName("annotationDialog");dialog.setWindowTitle(editing?QStringLiteral("编辑文字标注"):QStringLiteral("添加文字标注"));dialog.resize(440,300);
    auto *layout=new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("输入文字（支持中文和换行）")));
    auto *text=new QPlainTextEdit; text->setObjectName("annotationText");text->setPlaceholderText(QStringLiteral("例如：启动电流峰值\n24 V · 空载"));layout->addWidget(text,1);
    auto *style=new QHBoxLayout;style->addWidget(new QLabel(QStringLiteral("字号（像素）")));
    auto *size=new QSpinBox;size->setObjectName("annotationSize");size->setRange(10,120);size->setValue(editing?canvas_->annotations()[index].pixelSize:annotationSize_);style->addWidget(size);
    style->addWidget(new QLabel(QStringLiteral("颜色")));auto *color=new QComboBox;color->setObjectName("annotationColor");
    const QList<QPair<QString,QColor>> colors={{QStringLiteral("黄色"),QColor("#ffd43b")},{QStringLiteral("红色"),QColor("#ff6464")},{QStringLiteral("白色"),QColor("#ffffff")},{QStringLiteral("青色"),QColor("#35e6f2")},{QStringLiteral("绿色"),QColor("#65f18d")},{QStringLiteral("黑色"),QColor("#151a22")}};
    const QColor current=editing?canvas_->annotations()[index].color:annotationColor_;
    for(const auto &entry:colors){QPixmap icon(14,14);icon.fill(entry.second);color->addItem(QIcon(icon),entry.first,entry.second);if(entry.second==current)color->setCurrentIndex(color->count()-1);}
    style->addWidget(color);layout->addLayout(style);
    auto *error=new QLabel;error->setWordWrap(true);error->setStyleSheet("color:#b5433f");layout->addWidget(error);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));layout->addWidget(buttons);
    if(editing)text->setPlainText(canvas_->annotations()[index].text);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{
        const auto value=text->toPlainText();const auto selectedColor=color->currentData().value<QColor>();
        if(value.trimmed().isEmpty()){error->setText(QStringLiteral("请输入要标注的文字。"));return;}
        const bool ok=editing?canvas_->editText(index,value,size->value(),selectedColor):canvas_->addText(value,position,size->value(),selectedColor);
        if(!ok){error->setText(QStringLiteral("文字过长或超出图片尺寸，请减少字数、换行或减小字号（最多1000字）。"));return;}
        annotationSize_=size->value();annotationColor_=selectedColor;dialog.accept();
    });
    text->setFocus();dialog.exec();canvas_->setFocus();updateAnnotationActions();
}

bool MainWindow::confirmDiscardAnnotations() {
    if(!canvas_->hasUnsavedChanges())return true;
    QMessageBox box(QMessageBox::Question,QStringLiteral("保存文字标注"),QStringLiteral("当前文字标注有尚未保存的修改。是否先保存一张新图片？"),QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel,this);
    box.setObjectName("unsavedAnnotationsDialog");box.setDefaultButton(QMessageBox::Save);
    box.button(QMessageBox::Save)->setText(QStringLiteral("保存并继续"));box.button(QMessageBox::Discard)->setText(QStringLiteral("放弃修改"));box.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
    const auto answer=box.exec();
    if(answer==QMessageBox::Save)return saveImage(false);
    return answer==QMessageBox::Discard;
}
void MainWindow::about() {
    QMessageBox box(this);box.setWindowTitle(QStringLiteral("关于 ScopeSnap"));
    box.setText(QStringLiteral("ScopeSnap %1\n示波器截图工具").arg(APP_VERSION));
    box.setInformativeText(QStringLiteral("适用于 SIGLENT SDS3000X HD。支持 USB / LAN。\n\n本版已完成离线检查；具体仪器的首次截图仍需连接验证。\nUSB 需 VISA 驱动，网络连接不需要。随包 NI 安装器需联网，其使用及部署遵循 NI 独立许可。\n\n使用 Qt 6.5.3 动态库；许可证与对应源码随包提供。\n\nNI 组件：Copyright © 2026 National Instruments Corporation. All Rights Reserved."));
    auto *licenses=box.addButton(QStringLiteral("开源许可"),QMessageBox::ActionRole);box.addButton(QMessageBox::Ok);box.exec();
    if(box.clickedButton()==licenses) {
        auto path=QCoreApplication::applicationDirPath()+"/licenses";
        if(!QDir(path).exists())path=QCoreApplication::applicationDirPath()+"/../resources/licenses";
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}
void MainWindow::closeEvent(QCloseEvent *event) {
    if(busy_) { closeRequested_=true;centralWidget()->setEnabled(false);showMessage(QStringLiteral("等待当前通信结束后关闭…"));event->ignore();return; }
    if(!confirmDiscardAnnotations()){closeRequested_=false;centralWidget()->setEnabled(true);event->ignore();return;}
    event->accept();
}
