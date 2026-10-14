#include "ui/NewCanvasDialog.h"

#include "engine/CanvasEngine.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <algorithm>
#include <cmath>

namespace {

constexpr int kSmallestSide = 16;

QString trimmed(double v, int decimals)
{
    QString s = QString::number(v, 'f', decimals);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
    }
    return s;
}

} // namespace

double NewCanvasDialog::pixelsPerUnit(Unit unit, int dpi)
{
    switch (unit) {
    case Millimetres: return dpi / 25.4;
    case Centimetres: return dpi / 2.54;
    case Inches: return dpi;
    case Pixels: break;
    }
    return 1.0;
}

int NewCanvasDialog::finestResolution(double widthInches, double heightInches)
{
    if (widthInches <= 0.0 || heightInches <= 0.0)
        return 0;
    const double byArea = std::sqrt(double(CanvasEngine::maxCanvasPixels()) / (widthInches * heightInches));
    const double bySide = double(CanvasEngine::maxCanvasDimension()) / std::max(widthInches, heightInches);
    return int(std::floor(std::min(byArea, bySide)));
}

NewCanvasDialog::NewCanvasDialog(const QSize& screenPixels, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("New Canvas"));
    // The largest square this card holds, so the limit can be seen and picked rather than found by trying.
    const int largest = std::min(CanvasEngine::maxCanvasDimension(), int(std::floor(std::sqrt(double(CanvasEngine::maxCanvasPixels())))));
    m_presets = {
        {tr("Screen Size"), screenPixels},
        {tr("Square"), QSize(2048, 2048)},
        {tr("Large Square"), QSize(4096, 4096)},
        {tr("HD"), QSize(1920, 1080)},
        {tr("4K"), QSize(3840, 2160)},
        {tr("A4 at 300 dpi"), QSize(2480, 3508), 300, Millimetres},
        {tr("A3 at 300 dpi"), QSize(3508, 4961), 300, Millimetres},
        {tr("A2 at 300 dpi"), QSize(4961, 7016), 300, Millimetres},
        {tr("A1 at 300 dpi"), QSize(7016, 9933), 300, Millimetres},
        {tr("Poster 50 × 70 cm at 300 dpi"), QSize(5906, 8268), 300, Centimetres},
        {tr("1 m square at 150 dpi"), QSize(5906, 5906), 150, Centimetres},
        {tr("Comic Page"), QSize(4125, 6262)},
        {tr("Largest square on this card"), QSize(largest, largest)},
    };

    auto* form = new QFormLayout(this);
    form->setContentsMargins(20, 18, 20, 16);
    form->setVerticalSpacing(10);
    m_preset = new QComboBox(this);
    m_preset->setObjectName(QStringLiteral("canvas-preset"));
    for (const Preset& p : m_presets)
        m_preset->addItem(QStringLiteral("%1   %2 × %3").arg(p.name).arg(p.size.width()).arg(p.size.height()));
    m_preset->addItem(tr("Custom"));

    m_width = new QDoubleSpinBox(this);
    m_width->setObjectName(QStringLiteral("canvas-width"));
    m_height = new QDoubleSpinBox(this);
    m_height->setObjectName(QStringLiteral("canvas-height"));
    for (QDoubleSpinBox* box : {m_width, m_height}) {
        box->setMinimumWidth(130);
        box->setKeyboardTracking(false);   // a size typed in is taken when it is finished, not digit by digit
    }
    m_unit = new QComboBox(this);
    m_unit->setObjectName(QStringLiteral("canvas-unit"));
    m_unit->addItems({tr("pixels"), tr("millimetres"), tr("centimetres"), tr("inches")});
    m_dpi = new QSpinBox(this);
    m_dpi->setObjectName(QStringLiteral("canvas-dpi"));
    m_dpi->setRange(30, 1200);
    m_dpi->setSuffix(tr(" dpi"));
    m_dpi->setToolTip(tr("Dots per inch: how finely the picture is to be printed. 300 is usual for print seen close up, 150 for posters."));
    m_dpi->setKeyboardTracking(false);
    m_background = new QComboBox(this);
    m_background->addItems({tr("White"), tr("Current colour"), tr("Transparent")});

    auto* measure = new QHBoxLayout;
    measure->setSpacing(8);
    measure->addWidget(m_unit, 1);
    measure->addWidget(m_dpi);
    form->addRow(tr("Preset"), m_preset);
    form->addRow(tr("Width"), m_width);
    form->addRow(tr("Height"), m_height);
    form->addRow(tr("Measured in"), measure);
    form->addRow(tr("Background"), m_background);
    // Like Procreate's "maximum layers": how much this size leaves room for.
    m_room = new QLabel(this);
    m_room->setObjectName(QStringLiteral("canvas-room"));
    m_room->setWordWrap(true);
    m_room->setMinimumWidth(360);
    m_room->setMinimumHeight(m_room->fontMetrics().lineSpacing() * 3);
    m_room->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->addRow(QString(), m_room);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &NewCanvasDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    QSettings st;
    m_dpi->setValue(st.value(QStringLiteral("newCanvas/dpi"), 300).toInt());
    m_unit->setCurrentIndex(std::clamp(st.value(QStringLiteral("newCanvas/unit"), int(Pixels)).toInt(), int(Pixels), int(Inches)));
    m_px = screenPixels;
    showSize();
    markPreset();
    describe();

    connect(m_preset, &QComboBox::activated, this, &NewCanvasDialog::choosePreset);
    connect(m_width, &QDoubleSpinBox::valueChanged, this, &NewCanvasDialog::sizeEdited);
    connect(m_height, &QDoubleSpinBox::valueChanged, this, &NewCanvasDialog::sizeEdited);
    // Another unit says the same size differently.
    connect(m_unit, &QComboBox::currentIndexChanged, this, [this] {
        showSize();
        describe();
    });
    // Another resolution keeps a size on paper and changes the pixels; a size
    // in pixels stays as it is and comes out a different size on paper.
    connect(m_dpi, &QSpinBox::valueChanged, this, [this] {
        if (unit() == Pixels) {
            describe();
            return;
        }
        const double w = m_width->value(), h = m_height->value();
        showSize();   // the range follows the resolution
        setSize(w, h);
    });
}

int NewCanvasDialog::resolution() const
{
    return m_dpi->value();
}

NewCanvasDialog::Background NewCanvasDialog::background() const
{
    return Background(m_background->currentIndex());
}

bool NewCanvasDialog::fits() const
{
    return qint64(m_px.width()) * m_px.height() <= CanvasEngine::maxCanvasPixels() && m_px.width() <= CanvasEngine::maxCanvasDimension()
           && m_px.height() <= CanvasEngine::maxCanvasDimension();
}

NewCanvasDialog::Unit NewCanvasDialog::unit() const
{
    return Unit(m_unit->currentIndex());
}

void NewCanvasDialog::setUnit(Unit unit)
{
    m_unit->setCurrentIndex(int(unit));
}

void NewCanvasDialog::setResolution(int dpi)
{
    m_dpi->setValue(dpi);
}

void NewCanvasDialog::setSize(double width, double height)
{
    {
        const QSignalBlocker a(m_width), b(m_height);
        m_width->setValue(width);
        m_height->setValue(height);
    }
    sizeEdited();
}

double NewCanvasDialog::shownWidth() const
{
    return m_width->value();
}

double NewCanvasDialog::shownHeight() const
{
    return m_height->value();
}

QString NewCanvasDialog::roomText() const
{
    return m_room->text();
}

int NewCanvasDialog::presetCount() const
{
    return int(m_presets.size());
}

QString NewCanvasDialog::presetText(int index) const
{
    return m_preset->itemText(index);
}

int NewCanvasDialog::currentPreset() const
{
    return m_preset->currentIndex();
}

void NewCanvasDialog::choosePreset(int index)
{
    if (index < 0 || index >= int(m_presets.size()))
        return;
    const Preset& p = m_presets[index];
    {
        // A paper size brings its resolution with it, and shows in the measure it is known by.
        const QSignalBlocker a(m_dpi), b(m_unit);
        if (p.dpi > 0) {
            m_dpi->setValue(p.dpi);
            m_unit->setCurrentIndex(int(p.unit));
        } else {
            m_unit->setCurrentIndex(int(Pixels));
        }
    }
    m_px = p.size;
    showSize();
    markPreset();
    describe();
}

// Puts the pixel size into the boxes in the unit that is showing.
void NewCanvasDialog::showSize()
{
    const Unit u = unit();
    const double per = pixelsPerUnit(u, resolution());
    const int decimals = u == Pixels ? 0 : u == Millimetres ? 1 : u == Centimetres ? 2 : 3;
    const QString suffix = u == Pixels ? tr(" px") : u == Millimetres ? tr(" mm") : u == Centimetres ? tr(" cm") : tr(" in");
    const QSignalBlocker a(m_width), b(m_height);
    for (QDoubleSpinBox* box : {m_width, m_height}) {
        box->setDecimals(decimals);
        box->setSuffix(suffix);
        box->setSingleStep(u == Pixels ? 16.0 : u == Millimetres ? 5.0 : u == Centimetres ? 1.0 : 0.5);
        box->setRange(kSmallestSide / per, CanvasEngine::maxCanvasDimension() / per);
    }
    m_width->setValue(m_px.width() / per);
    m_height->setValue(m_px.height() / per);
}

void NewCanvasDialog::sizeEdited()
{
    const double per = pixelsPerUnit(unit(), resolution());
    const int limit = CanvasEngine::maxCanvasDimension();
    m_px = QSize(std::clamp(int(std::lround(m_width->value() * per)), kSmallestSide, limit),
                 std::clamp(int(std::lround(m_height->value() * per)), kSmallestSide, limit));
    markPreset();
    describe();
}

void NewCanvasDialog::markPreset()
{
    int found = int(m_presets.size());
    for (int i = 0; i < int(m_presets.size()); ++i) {
        if (m_presets[i].size == m_px && (m_presets[i].dpi == 0 || m_presets[i].dpi == resolution())) {
            found = i;
            break;
        }
    }
    m_preset->setCurrentIndex(found);
}

void NewCanvasDialog::describe()
{
    const qint64 pixels = qint64(m_px.width()) * m_px.height();
    const int dpi = resolution();
    const QString megapixels = QString::number(double(pixels) / 1e6, 'f', pixels < 10'000'000 ? 1 : 0);
    const double widthInches = double(m_px.width()) / dpi, heightInches = double(m_px.height()) / dpi;
    // Whichever of the two the boxes do not show already.
    const QString other = unit() == Pixels
                              ? tr("%1 × %2 cm at %3 dpi").arg(trimmed(widthInches * 2.54, 1), trimmed(heightInches * 2.54, 1)).arg(dpi)
                              : tr("%1 × %2 pixels").arg(m_px.width()).arg(m_px.height());
    const int layers = CanvasEngine::layerEstimate(m_px);
    QString text;
    if (!fits()) {
        const int finest = finestResolution(widthInches, heightInches);
        text = tr("%1 · %2 megapixels: more than this graphics card can hold (the limit here is %3).")
                   .arg(other, megapixels).arg(CanvasEngine::maxCanvasPixels() / 1'000'000);
        if (finest >= 30)
            text += QLatin1Char(' ') + tr("A sheet this size fits at up to %1 dpi.").arg(finest);
    } else {
        // Where a layer only takes memory for the paint on it, the count is
        // for layers painted from edge to edge: the fewest there is room for.
        if (!CanvasEngine::sparseLayers())
            text = tr("%1 · %2 megapixels · room for about %3 layers in graphics memory").arg(other, megapixels).arg(layers);
        else if (layers <= 1)
            text = tr("%1 · %2 megapixels · room for the paint of about one layer filled edge to edge; layers with less on them take less").arg(other, megapixels);
        else
            text = tr("%1 · %2 megapixels · room for about %3 layers filled edge to edge; layers with less on them take less").arg(other, megapixels).arg(layers);
    }
    m_room->setText(text);
    m_room->setStyleSheet(!fits() || layers < 3 ? QStringLiteral("color: #ff9f0a;") : QStringLiteral("color: #a0a0a8;"));
}

void NewCanvasDialog::accept()
{
    // What is still being typed counts.
    m_width->interpretText();
    m_height->interpretText();
    m_dpi->interpretText();
    if (!fits())
        return;   // the line under the boxes says why, and what would fit
    QSettings st;
    st.setValue(QStringLiteral("newCanvas/dpi"), resolution());
    st.setValue(QStringLiteral("newCanvas/unit"), int(unit()));
    QDialog::accept();
}
