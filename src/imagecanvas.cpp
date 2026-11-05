#include "imagecanvas.h"
#include <QApplication>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

ImageCanvas::ImageCanvas(QWidget *parent) : QWidget(parent) {
    setObjectName("imageCanvas");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
}

void ImageCanvas::setImage(const QImage &image) {
    image_ = image;
    annotations_.clear(); saved_.clear(); undo_.clear(); dragBefore_.clear();
    selected_ = -1; dragging_ = false; dragStarted_ = false; pendingAdd_ = false;
    setAddMode(false);
    updateSize(); update();
    emit annotationsChanged(); emit selectionChanged();
}

void ImageCanvas::setActual(bool actual) {
    finishDrag(); actual_ = actual; updateSize(); update();
}

void ImageCanvas::setAddMode(bool enabled) {
    addMode_ = enabled && !image_.isNull();
    setCursor(addMode_ ? Qt::CrossCursor : Qt::ArrowCursor);
}

void ImageCanvas::updateSize() {
    setMinimumSize(actual_ && !image_.isNull() ? image_.size() + QSize(40,40) : QSize(0,0));
}

QRectF ImageCanvas::imageRect() const {
    if (image_.isNull()) return {};
    QSize size = image_.size();
    if (!actual_) size.scale((this->size()-QSize(40,40)).expandedTo(QSize(1,1)),Qt::KeepAspectRatio);
    return QRectF(QPointF((width()-size.width())/2.0,(height()-size.height())/2.0),size);
}

QPointF ImageCanvas::widgetToImage(const QPointF &point) const {
    const auto dest = imageRect();
    if (dest.isEmpty()) return {};
    return {(point.x()-dest.left())*image_.width()/dest.width(),
            (point.y()-dest.top())*image_.height()/dest.height()};
}

QPointF ImageCanvas::imageToWidget(const QPointF &point) const {
    const auto dest = imageRect();
    if (dest.isEmpty()) return {};
    return {dest.left()+point.x()*dest.width()/image_.width(),
            dest.top()+point.y()*dest.height()/image_.height()};
}

QPainterPath ImageCanvas::textPath(const TextAnnotation &annotation) {
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPixelSize(annotation.pixelSize); font.setBold(true);
    const QFontMetricsF metrics(font);
    QPainterPath path;
    qreal baseline = metrics.ascent();
    for (const auto &line : annotation.text.split('\n')) {
        path.addText(QPointF(0,baseline),font,line);
        baseline += metrics.lineSpacing();
    }
    const auto bounds = path.boundingRect();
    path.translate(3-bounds.left(),3-bounds.top());
    return path;
}

QSizeF ImageCanvas::textSize(const TextAnnotation &annotation) {
    const auto bounds = textPath(annotation).boundingRect();
    return {bounds.width()+6,bounds.height()+6};
}

bool ImageCanvas::validAnnotation(const TextAnnotation &annotation) const {
    if (image_.isNull() || annotation.text.trimmed().isEmpty() || annotation.text.size()>1000
        || annotation.pixelSize<10 || annotation.pixelSize>120 || !annotation.color.isValid()) return false;
    const auto size = textSize(annotation);
    return size.width()<=image_.width() && size.height()<=image_.height();
}

QPointF ImageCanvas::clampedPosition(const TextAnnotation &annotation,const QPointF &position) const {
    const auto size = textSize(annotation);
    return {qBound(0.0,position.x(),qMax(0.0,image_.width()-size.width())),
            qBound(0.0,position.y(),qMax(0.0,image_.height()-size.height()))};
}

QRectF ImageCanvas::annotationBounds(int index) const {
    if (index<0 || index>=annotations_.size()) return {};
    return {annotations_[index].position,textSize(annotations_[index])};
}

void ImageCanvas::remember(const QVector<TextAnnotation> &before) {
    undo_.append(before);
    if (undo_.size()>50) undo_.removeFirst();
}

bool ImageCanvas::addText(const QString &text,const QPointF &position,int pixelSize,const QColor &color) {
    TextAnnotation annotation{text.trimmed(),position,pixelSize,color};
    if (!validAnnotation(annotation)) return false;
    finishDrag(); remember(annotations_);
    annotation.position = clampedPosition(annotation,position);
    annotations_.append(annotation); selected_ = int(annotations_.size())-1;
    setAddMode(false); update(); emit annotationsChanged(); emit selectionChanged(); return true;
}

bool ImageCanvas::editText(int index,const QString &text,int pixelSize,const QColor &color) {
    if (index<0 || index>=annotations_.size()) return false;
    TextAnnotation annotation{text.trimmed(),annotations_[index].position,pixelSize,color};
    if (!validAnnotation(annotation)) return false;
    annotation.position = clampedPosition(annotation,annotation.position);
    if (annotation == annotations_[index]) return true;
    finishDrag(); remember(annotations_); annotations_[index] = annotation;
    update(); emit annotationsChanged(); return true;
}

void ImageCanvas::deleteSelected() {
    if (selected_<0 || selected_>=annotations_.size()) return;
    finishDrag(); remember(annotations_); annotations_.removeAt(selected_); selected_ = -1;
    update(); emit annotationsChanged(); emit selectionChanged();
}

void ImageCanvas::clearAnnotations() {
    if (annotations_.isEmpty()) return;
    finishDrag(); remember(annotations_); annotations_.clear(); selected_ = -1;
    update(); emit annotationsChanged(); emit selectionChanged();
}

void ImageCanvas::undo() {
    finishDrag();
    if (undo_.isEmpty()) return;
    annotations_ = undo_.takeLast(); selected_ = -1;
    update(); emit annotationsChanged(); emit selectionChanged();
}

void ImageCanvas::markSaved() {
    finishDrag(); saved_ = annotations_; emit annotationsChanged();
}

void ImageCanvas::drawAnnotations(QPainter &painter) const {
    painter.setRenderHint(QPainter::Antialiasing);
    for (const auto &annotation : annotations_) {
        painter.save(); painter.translate(annotation.position);
        painter.setPen(QPen(QColor(12,18,28,230),2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        painter.setBrush(annotation.color); painter.drawPath(textPath(annotation)); painter.restore();
    }
}

QImage ImageCanvas::renderedImage() const {
    if (image_.isNull() || annotations_.isEmpty()) return image_;
    QImage result = image_.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&result); drawAnnotations(painter); return result;
}

void ImageCanvas::paintEvent(QPaintEvent *) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(),QColor("#eef2f7"));
    if (!image_.isNull()) {
        const auto dest = imageRect();
        painter.fillRect(dest.adjusted(-1,-1,1,1),QColor("#cdd6e3"));
        painter.setRenderHint(QPainter::SmoothPixmapTransform,!actual_);
        painter.drawImage(dest,image_);
        painter.save(); painter.setClipRect(dest); painter.translate(dest.topLeft());
        painter.scale(dest.width()/image_.width(),dest.height()/image_.height());
        drawAnnotations(painter); painter.restore();
        if (selected_>=0) {
            const auto bounds = annotationBounds(selected_);
            painter.setBrush(Qt::NoBrush); painter.setPen(QPen(QColor("#2879f0"),1,Qt::DashLine));
            painter.drawRect(QRectF(imageToWidget(bounds.topLeft()),imageToWidget(bounds.bottomRight())).adjusted(-2,-2,2,2));
        }
        return;
    }
    const QPoint center(width()/2,height()/2-35);
    painter.setPen(QPen(QColor("#b4c3d8"),2)); painter.setBrush(QColor("#f8faff"));
    painter.drawRoundedRect(QRect(center.x()-46,center.y()-39,92,65),9,9);
    QPainterPath path; path.moveTo(center.x()-33,center.y()-3); path.lineTo(center.x()-19,center.y()-3);
    path.lineTo(center.x()-10,center.y()-21); path.lineTo(center.x()+2,center.y()+12);
    path.lineTo(center.x()+13,center.y()-10); path.lineTo(center.x()+32,center.y()-10);
    painter.setPen(QPen(QColor("#4385e6"),3,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin)); painter.drawPath(path);
    painter.setPen(QColor("#35455c")); QFont font = painter.font(); font.setPointSize(15); font.setBold(true); painter.setFont(font);
    painter.drawText(QRect(0,center.y()+49,width(),34),Qt::AlignCenter,QStringLiteral("把示波器屏幕留在电脑上"));
    font.setPointSize(10); font.setBold(false); painter.setFont(font); painter.setPen(QColor("#7d8b9e"));
    painter.drawText(QRect(15,center.y()+89,width()-30,50),Qt::AlignHCenter|Qt::AlignTop,
                     QStringLiteral("连接设备后点击「截图」\n暂时没有设备？选择左侧「离线演示」体验操作。"));
}

int ImageCanvas::hitTest(const QPointF &position) const {
    for (int i=int(annotations_.size())-1;i>=0;--i) if (annotationBounds(i).contains(position)) return i;
    return -1;
}

void ImageCanvas::mousePressEvent(QMouseEvent *event) {
    if (event->button()!=Qt::LeftButton || image_.isNull()) return;
    setFocus(); finishDrag(); pressPosition_ = event->position();
    pendingAdd_ = false;
    selected_ = imageRect().contains(pressPosition_) ? hitTest(widgetToImage(pressPosition_)) : -1;
    if (selected_>=0) {
        dragging_ = true; dragStarted_ = false; dragBefore_ = annotations_; dragPosition_ = annotations_[selected_].position;
    } else pendingAdd_ = addMode_ && imageRect().contains(pressPosition_);
    update(); emit selectionChanged();
}

void ImageCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (!dragging_ || !(event->buttons()&Qt::LeftButton)) return;
    if (!dragStarted_ && (event->position()-pressPosition_).manhattanLength()<QApplication::startDragDistance()) return;
    dragStarted_ = true;
    const auto delta = widgetToImage(event->position())-widgetToImage(pressPosition_);
    annotations_[selected_].position = clampedPosition(annotations_[selected_],dragPosition_+delta);
    update(); emit annotationsChanged();
}

void ImageCanvas::finishDrag() {
    if (!dragging_) return;
    dragging_ = false; dragStarted_ = false;
    if (dragBefore_!=annotations_) { remember(dragBefore_); emit annotationsChanged(); }
    dragBefore_.clear();
}

void ImageCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button()!=Qt::LeftButton) return;
    finishDrag();
    if (pendingAdd_ && imageRect().contains(event->position())
        && (event->position()-pressPosition_).manhattanLength()<QApplication::startDragDistance()) {
        pendingAdd_ = false; emit addRequested(widgetToImage(event->position()));
    }
    pendingAdd_ = false;
}

void ImageCanvas::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button()!=Qt::LeftButton || image_.isNull()) return;
    finishDrag(); pendingAdd_ = false;
    selected_ = hitTest(widgetToImage(event->position()));
    if (selected_>=0) { emit selectionChanged(); emit editRequested(selected_); }
}

void ImageCanvas::keyPressEvent(QKeyEvent *event) {
    if (event->key()==Qt::Key_Delete) { deleteSelected(); return; }
    if (event->matches(QKeySequence::Undo)) { undo(); return; }
    if (event->key()==Qt::Key_Escape) {
        if (dragging_) { annotations_ = dragBefore_; dragging_ = false; dragStarted_ = false; dragBefore_.clear(); emit annotationsChanged(); }
        pendingAdd_ = false; setAddMode(false); selected_ = -1; update(); emit selectionChanged(); return;
    }
    if ((event->key()==Qt::Key_Return || event->key()==Qt::Key_Enter) && selected_>=0) { emit editRequested(selected_); return; }
    QWidget::keyPressEvent(event);
}

void ImageCanvas::changeEvent(QEvent *event) {
    if (event->type()==QEvent::EnabledChange && !isEnabled()) { finishDrag(); pendingAdd_ = false; }
    QWidget::changeEvent(event);
}
