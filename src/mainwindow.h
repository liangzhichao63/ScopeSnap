#pragma once
#include <QMainWindow>
#include <QImage>
#include <QThread>

class ScopeWorker;
class ImageCanvas;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QPushButton;
class QLabel;
class QStackedWidget;
class QListWidget;
class QCheckBox;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(bool startDemo = false, QWidget *parent = nullptr);
    ~MainWindow() override;
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void buildUi();
    void scan();
    void toggleConnection();
    void capture();
    void saveImage(bool choosePath);
    void copyImage();
    void setBusy(bool busy, const QString &message = {});
    void updateActions();
    void showMessage(const QString &message, bool error = false);
    void onImage(const QImage &image, bool demo);
    void about();
    QString outputDirectory() const;
    QString suggestedFilename() const;
    ScopeWorker *worker_ = nullptr;
    QThread thread_;
    ImageCanvas *canvas_ = nullptr;
    QComboBox *mode_ = nullptr;
    QComboBox *devices_ = nullptr;
    QLineEdit *host_ = nullptr;
    QSpinBox *port_ = nullptr;
    QLineEdit *directory_ = nullptr;
    QLineEdit *prefix_ = nullptr;
    QCheckBox *autoSave_ = nullptr;
    QStackedWidget *connectionPages_ = nullptr;
    QPushButton *scanButton_ = nullptr;
    QPushButton *connectButton_ = nullptr;
    QPushButton *captureButton_ = nullptr;
    QPushButton *saveButton_ = nullptr;
    QPushButton *copyButton_ = nullptr;
    QLabel *statusBadge_ = nullptr;
    QLabel *deviceInfo_ = nullptr;
    QLabel *message_ = nullptr;
    QLabel *imageInfo_ = nullptr;
    QListWidget *history_ = nullptr;
    QImage image_;
    bool connected_ = false;
    bool demo_ = false;
    bool imageDemo_ = false;
    bool busy_ = false;
    bool closeRequested_ = false;
};
