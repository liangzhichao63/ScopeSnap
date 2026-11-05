#pragma once
#include <QMainWindow>
#include <QImage>
#include <QColor>
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
    bool saveImage(bool choosePath);
    void copyImage();
    void editAnnotation(const QPointF &position, int index = -1);
    bool confirmDiscardAnnotations();
    void updateAnnotationActions();
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
    QPushButton *addTextButton_ = nullptr;
    QPushButton *editTextButton_ = nullptr;
    QPushButton *deleteTextButton_ = nullptr;
    QPushButton *undoTextButton_ = nullptr;
    QPushButton *clearTextButton_ = nullptr;
    QPushButton *saveAnnotationsButton_ = nullptr;
    QLabel *annotationStatus_ = nullptr;
    int annotationSize_ = 28;
    QColor annotationColor_ = QColor("#ffd43b");
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
