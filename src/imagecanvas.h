#pragma once
#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QVector>
#include <QWidget>

struct TextAnnotation {
    QString text;
    QPointF position;
    int pixelSize = 28;
    QColor color = QColor("#ffd43b");
    bool operator==(const TextAnnotation &other) const {
        return text == other.text && position == other.position
            && pixelSize == other.pixelSize && color == other.color;
    }
};

class ImageCanvas : public QWidget {
    Q_OBJECT
public:
    explicit ImageCanvas(QWidget *parent = nullptr);
    void setImage(const QImage &image);
    void setActual(bool actual);
    void setAddMode(bool enabled);
    bool addMode() const { return addMode_; }
    QRectF imageRect() const;
    QPointF imageToWidget(const QPointF &point) const;
    QImage renderedImage() const;
    const QImage &sourceImage() const { return image_; }
    const QVector<TextAnnotation> &annotations() const { return annotations_; }
    int selectedIndex() const { return selected_; }
    QRectF annotationBounds(int index) const;
    bool addText(const QString &text, const QPointF &position, int pixelSize, const QColor &color);
    bool editText(int index, const QString &text, int pixelSize, const QColor &color);
    void deleteSelected();
    void clearAnnotations();
    void undo();
    bool canUndo() const { return !undo_.isEmpty(); }
    bool hasUnsavedChanges() const { return annotations_ != saved_; }
    void markSaved();
signals:
    void addRequested(const QPointF &position);
    void editRequested(int index);
    void annotationsChanged();
    void selectionChanged();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;
private:
    static QPainterPath textPath(const TextAnnotation &annotation);
    static QSizeF textSize(const TextAnnotation &annotation);
    QPointF widgetToImage(const QPointF &point) const;
    QPointF clampedPosition(const TextAnnotation &annotation, const QPointF &position) const;
    bool validAnnotation(const TextAnnotation &annotation) const;
    int hitTest(const QPointF &position) const;
    void drawAnnotations(QPainter &painter) const;
    void updateSize();
    void remember(const QVector<TextAnnotation> &before);
    void finishDrag();
    QImage image_;
    QVector<TextAnnotation> annotations_;
    QVector<TextAnnotation> saved_;
    QVector<QVector<TextAnnotation>> undo_;
    QVector<TextAnnotation> dragBefore_;
    QPointF pressPosition_;
    QPointF dragPosition_;
    int selected_ = -1;
    bool actual_ = false;
    bool addMode_ = false;
    bool dragging_ = false;
    bool dragStarted_ = false;
    bool pendingAdd_ = false;
};
