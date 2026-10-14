// Text typed straight onto the canvas. A hidden editor takes the keys, so
// everything a text field does (accents, paste, undo, select all) works; what
// it holds is drawn on the canvas at the size and place it will have.

#include "engine/CanvasEngine.h"
#include "ui/CanvasWidget.h"
#include "ui/Theme.h"
#include "ui/ToolState.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <cmath>

void CanvasWidget::setTextMode(bool on)
{
    if (m_text.mode == on)
        return;
    if (on) {
        if (m_tf.active || m_shape.editing || m_crop.active || m_liquify.active || m_engine->editActive() || m_drag != Drag::None
            || !m_engine->hasDocument())
            return;
        setSelectMode(false);
        setGradientMode(false);
        armFill(false);
        armEyedropper(false);
        withdrawShapeOffer();
        if (!m_text.editor) {
            // Out of sight, but a real widget: it must be shown to hold the keyboard.
            m_text.editor = new QPlainTextEdit(this);
            m_text.editor->setObjectName(QStringLiteral("text-editor"));
            m_text.editor->setGeometry(0, 0, 1, 1);
            m_text.editor->setFrameShape(QFrame::NoFrame);
            m_text.editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            m_text.editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            m_text.editor->setLineWrapMode(QPlainTextEdit::NoWrap);
            m_text.editor->setStyleSheet(QStringLiteral("background: transparent; color: transparent; border: none;"));
            connect(m_text.editor, &QPlainTextEdit::textChanged, this, [this] {
                m_text.caretOn = true;
                update();
                emit textChanged();
            });
            connect(m_text.editor, &QPlainTextEdit::cursorPositionChanged, this, [this] {
                m_text.caretOn = true;
                update();
            });
            m_text.blink.setInterval(530);
            connect(&m_text.blink, &QTimer::timeout, this, [this] {
                m_text.caretOn = !m_text.caretOn;
                update();
            });
        }
        if (m_text.font.pixelSize() <= 0)
            m_text.font.setPixelSize(std::max(24, m_engine->canvasSize().height() / 12));
        m_text.editor->clear();
        m_text.editor->show();
        m_text.placed = false;
        m_text.editLayer = 0;
        m_text.matrix = QTransform();
        m_text.blink.start();
    } else {
        m_text.blink.stop();
        // A layer that was being retyped comes back into the picture, changed or not.
        m_engine->setTextEditLayer(0);
        m_text.editLayer = 0;
        m_text.matrix = QTransform();
        if (m_drag == Drag::Text)
            m_drag = Drag::None;
        if (m_text.editor)
            m_text.editor->hide();
        setFocus();
    }
    m_text.mode = on;
    updateCursorShape();
    update();
    emit textModeChanged(on);
}

void CanvasWidget::setTextFont(const QFont& font)
{
    m_text.font = font;
    m_text.font.setStyleStrategy(QFont::PreferAntialias);
    m_text.font.setHintingPreference(QFont::PreferNoHinting);   // letter shapes as designed, at any size
    update();
}

void CanvasWidget::setTextAlignment(Qt::Alignment alignment)
{
    m_text.alignment = alignment;
    update();
}

void CanvasWidget::setTextSpacing(double letters, double lines)
{
    m_text.letterSpacing = std::clamp(letters, -0.5, 2.0);
    m_text.lineSpacing = std::clamp(lines, 0.3, 4.0);
    update();
}

QString CanvasWidget::textContent() const
{
    return m_text.editor ? m_text.editor->toPlainText() : QString();
}

TextInfo CanvasWidget::textInfo() const
{
    TextInfo info;
    info.text = textContent();
    info.setFont(m_text.font);
    info.alignment = int(m_text.alignment);
    info.letterSpacing = m_text.letterSpacing;
    info.lineSpacing = m_text.lineSpacing;
    info.color = m_tools->color();
    info.anchor = m_text.anchor;
    info.matrix = m_text.matrix;
    return info;
}

// Where every line sits: the anchor is the top of the first line, at its
// left, middle or right depending on the alignment. This is where the text
// was typed; a text that has since been moved or turned has a matrix on top.
QVector<TextLine> CanvasWidget::textLines(QRectF* bounds, double* lineHeight) const
{
    return text::layout(textInfo(), bounds, lineHeight);
}

QPointF CanvasWidget::toTextSpace(const QPointF& widgetPos) const
{
    return m_text.matrix.inverted().map(widgetToCanvas(widgetPos));
}

// Opens the text of a layer for retyping, in the font, colour and place it has.
bool CanvasWidget::editTextLayer(int layerId)
{
    if (!m_engine->layerIsText(layerId))
        return false;
    if (!m_text.mode)
        setTextMode(true);
    if (!m_text.mode)
        return false;
    const TextInfo info = m_engine->layerText(layerId);
    m_engine->setActiveLayer(layerId);
    m_engine->setTextEditLayer(layerId);   // what is typed is shown in its place
    m_text.editLayer = layerId;
    m_text.matrix = info.matrix;
    m_text.anchor = info.anchor;
    m_text.alignment = Qt::Alignment(info.alignment);
    m_text.letterSpacing = info.letterSpacing;
    m_text.lineSpacing = info.lineSpacing;
    setTextFont(info.font());
    m_tools->setColor(info.color);
    m_text.editor->setPlainText(info.text);
    m_text.editor->moveCursor(QTextCursor::End);
    m_text.editor->setFocus();
    m_text.placed = true;
    m_text.caretOn = true;
    emit textStyleChanged();
    emit textChanged();
    update();
    return true;
}

void CanvasWidget::textPress(const PointerInput& in)
{
    if (!m_text.placed) {
        // On text that is already there, the tap opens that text.
        const int there = m_engine->textLayerAt(widgetToCanvas(in.pos));
        if (there && editTextLayer(there))
            return;
    }
    const QPointF at = toTextSpace(in.pos);
    if (!m_text.placed) {
        // The caret appears where the pen came down, centred on it.
        m_text.anchor = QPointF(std::round(at.x()), std::round(at.y() - QFontMetricsF(m_text.font).lineSpacing() / 2.0));
        m_text.placed = true;
    }
    // Pressing again, anywhere, picks the text up to be moved.
    m_text.grab = m_text.anchor - at;
    m_drag = Drag::Text;
    m_text.caretOn = true;
    m_text.editor->setFocus();
    updateCursorShape();
    update();
}

void CanvasWidget::textMove(const PointerInput& in)
{
    const QPointF to = toTextSpace(in.pos) + m_text.grab;
    m_text.anchor = QPointF(std::round(to.x()), std::round(to.y()));
    update();
}

void CanvasWidget::drawTextOverlay(QPainter& p)
{
    if (!m_text.mode || !m_text.placed)
        return;
    QRectF bounds;
    double lineHeight = 0.0;
    const QVector<TextLine> lines = textLines(&bounds, &lineHeight);
    const double dpr = devicePixelRatioF();
    const QTransform toWidget = m_text.matrix * viewTransform() * QTransform::fromScale(1.0 / dpr, 1.0 / dpr);
    const QTextCursor cursor = m_text.editor->textCursor();

    p.save();
    p.setTransform(toWidget, true);
    // What is selected, behind the letters.
    // The letters as they will be set, spacing and all.
    const QFont font = textInfo().font();
    if (cursor.hasSelection()) {
        const QFontMetricsF metrics(font);
        for (const TextLine& line : lines) {
            const int from = std::max(cursor.selectionStart(), line.start), to = std::min(cursor.selectionEnd(), line.start + int(line.text.size()));
            if (from > to || cursor.selectionEnd() < line.start || cursor.selectionStart() > line.start + line.text.size())
                continue;
            const double x0 = line.x + metrics.horizontalAdvance(line.text.left(from - line.start));
            const double x1 = line.x + metrics.horizontalAdvance(line.text.left(to - line.start));
            p.fillRect(QRectF(x0, line.top, std::max(x1 - x0, lineHeight * 0.15), lineHeight), QColor(10, 132, 255, 90));
        }
    }
    p.setFont(font);
    p.setPen(m_tools->color());
    for (const TextLine& line : lines)
        p.drawText(QPointF(line.x, line.baseline), line.text);
    p.restore();

    // The caret and a faint frame, at screen thickness whatever the zoom.
    QPolygonF frame = toWidget.map(QPolygonF(bounds.adjusted(-6, -4, 6, 4)));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 150), 1, Qt::DashLine));
    p.drawPolygon(frame);
    p.setPen(QPen(QColor(0, 0, 0, 90), 1, Qt::DashLine));
    p.drawPolygon(frame.translated(1, 1));
    if (m_text.caretOn) {
        const QFontMetricsF metrics(font);
        for (const TextLine& line : lines) {
            const int at = cursor.position() - line.start;
            if (at < 0 || at > line.text.size())
                continue;
            const double x = line.x + metrics.horizontalAdvance(line.text.left(at));
            const QPointF top = toWidget.map(QPointF(x, line.top)), bottom = toWidget.map(QPointF(x, line.top + lineHeight));
            p.setPen(QPen(QColor(255, 255, 255, 220), 3.2));
            p.drawLine(top, bottom);
            p.setPen(QPen(theme::accent, 1.6));
            p.drawLine(top, bottom);
            break;
        }
    }
}

// New text is handed over to become a layer; text that was being retyped is
// set afresh on its own layer.
bool CanvasWidget::commitText()
{
    if (!m_text.mode)
        return false;
    const TextInfo info = textInfo();
    if (!m_text.placed || info.text.trimmed().isEmpty()) {
        setTextMode(false);
        return false;
    }
    QRectF bounds;
    text::layout(info, &bounds);
    const QRectF covered = info.matrix.mapRect(bounds);
    const int limit = CanvasEngine::maxCanvasDimension();
    if (covered.width() > limit || covered.height() > limit) {
        emit toast(tr("That text is too large to fit a layer"));
        return false;
    }
    QString name = info.text.simplified();
    if (name.size() > 24)
        name = name.left(23) + QChar(0x2026);
    m_tools->noteColorUsed();
    const int layer = m_text.editLayer;
    setTextMode(false);
    if (layer)
        return m_engine->setLayerText(layer, info);
    emit textCommitted(info, name);
    return true;
}
