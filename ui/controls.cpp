#include "controls.h"

#include <QIconEngine>
#include <QPainter>
#include <QSvgRenderer>
#include <QVariantAnimation>
#include <algorithm>
#include <iterator>
#include <memory>

namespace ashdesk {
namespace {

constexpr qreal kTrackWidth = 40;
constexpr qreal kTrackHeight = 24;
constexpr qreal kKnob = 18;
constexpr qreal kGap = 12;
constexpr qreal kRingInset = 3;

struct Symbol {
    const char *name;
    const char *body;
};

constexpr Symbol kSymbols[] = {
    {"link", R"(<path d="M10 14a4 4 0 0 0 5.66 0l3-3a4 4 0 0 0-5.66-5.66l-1 1"/><path d="M14 10a4 4 0 0 0-5.66 0l-3 3a4 4 0 0 0 5.66 5.66l1-1"/>)"},
    {"monitor", R"(<rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8M12 16v4"/>)"},
    {"server", R"(<rect x="4" y="4" width="16" height="6" rx="1.5"/><rect x="4" y="14" width="16" height="6" rx="1.5"/><path d="M8 7h.01M8 17h.01"/>)"},
    {"devices", R"(<rect x="2.5" y="4" width="13" height="10" rx="1.5"/><path d="M6 18.5h6M9 14v4.5"/><rect x="17.5" y="8" width="4" height="11" rx="1"/>)"},
    {"settings", R"(<path d="M4 6h9M18 6h2M4 12h3M11 12h9M4 18h11M19 18h1"/><circle cx="15.5" cy="6" r="2.2"/><circle cx="9" cy="12" r="2.2"/><circle cx="17" cy="18" r="2.2"/>)"},
    {"search", R"(<circle cx="11" cy="11" r="6.5"/><path d="m16 16 4.5 4.5"/>)"},
    {"power", R"(<path d="M12 3v8"/><path d="M6.3 6.8a8 8 0 1 0 11.4 0"/>)"},
    {"plus", R"(<path d="M12 5v14M5 12h14"/>)"},
    {"trash", R"(<path d="M4 7h16M10 7V4.5h4V7M6.5 7l1 13h9l1-13"/>)"},
    {"folder", R"(<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/>)"},
    {"archive", R"(<rect x="3" y="4" width="18" height="4.5" rx="1"/><path d="M5 8.5V18a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8.5M10 12.5h4"/>)"},
    {"pulse", R"(<path d="M3 12h4l3-7 4 14 3-7h4"/>)"},
    {"copy", R"(<rect x="9" y="9" width="11" height="11" rx="2"/><path d="M5 15V6a2 2 0 0 1 2-2h9"/>)"},
    {"eye", R"(<path d="M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12Z"/><circle cx="12" cy="12" r="2.8"/>)"},
    {"refresh", R"(<path d="M20 11a8 8 0 0 0-14.6-3.5M4 4v4h4"/><path d="M4 13a8 8 0 0 0 14.6 3.5M20 20v-4h-4"/>)"},
};

class SvgIconEngine final : public QIconEngine {
public:
    SvgIconEngine(const QByteArray &off, const QByteArray &on, const QByteArray &disabled)
        : off_(std::make_shared<QSvgRenderer>(off)), on_(on.isEmpty() ? off_ : std::make_shared<QSvgRenderer>(on)),
          disabled_(disabled.isEmpty() ? nullptr : std::make_shared<QSvgRenderer>(disabled)) {}

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        QSvgRenderer *renderer = (state == QIcon::On ? on_ : off_).get();
        if (mode == QIcon::Disabled && disabled_) renderer = disabled_.get();
        else if (mode == QIcon::Disabled) painter->setOpacity(painter->opacity() * .45);
        renderer->render(painter, QRectF(rect));
        painter->restore();
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        QPixmap image(size * scale);
        image.setDevicePixelRatio(scale);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        return image;
    }

    QIconEngine *clone() const override { return new SvgIconEngine(*this); }
    QString key() const override { return QStringLiteral("ashdesk-svg"); }

private:
    std::shared_ptr<QSvgRenderer> off_;
    std::shared_ptr<QSvgRenderer> on_;
    std::shared_ptr<QSvgRenderer> disabled_;
};

QByteArray strokeSvg(const char *body, const QColor &color) {
    return QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\"><g fill=\"none\" "
                          "stroke=\"%1\" stroke-opacity=\"%2\" stroke-width=\"1.75\" stroke-linecap=\"round\" "
                          "stroke-linejoin=\"round\">%3</g></svg>")
        .arg(color.name(), QString::number(color.alphaF()), QLatin1String(body))
        .toUtf8();
}

} // namespace

FlowLayout::FlowLayout(int spacing) {
    setContentsMargins(0, 0, 0, 0);
    setSpacing(spacing);
}

FlowLayout::~FlowLayout() { qDeleteAll(items_); }

void FlowLayout::addItem(QLayoutItem *item) { items_.append(item); }
int FlowLayout::count() const { return int(items_.size()); }
QLayoutItem *FlowLayout::itemAt(int index) const { return items_.value(index); }
QLayoutItem *FlowLayout::takeAt(int index) { return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr; }
bool FlowLayout::hasHeightForWidth() const { return true; }
int FlowLayout::heightForWidth(int width) const { return arrange(QRect(0, 0, width, 0), false); }
QSize FlowLayout::sizeHint() const { return minimumSize(); }

QSize FlowLayout::minimumSize() const {
    QSize size;
    for (const QLayoutItem *item : items_) size = size.expandedTo(item->minimumSize());
    return size;
}

void FlowLayout::setGeometry(const QRect &rect) {
    QLayout::setGeometry(rect);
    arrange(rect, true);
}

int FlowLayout::arrange(const QRect &rect, bool apply) const {
    int x = rect.x();
    int y = rect.y();
    int rowHeight = 0;
    for (QLayoutItem *item : items_) {
        const QSize size = item->sizeHint();
        if (x > rect.x() && x + size.width() > rect.x() + rect.width()) {
            x = rect.x();
            y += rowHeight + spacing();
            rowHeight = 0;
        }
        if (apply) item->setGeometry(QRect(QPoint(x, y), size));
        x += size.width() + spacing();
        rowHeight = std::max(rowHeight, size.height());
    }
    return y + rowHeight - rect.y();
}

ToggleSwitch::ToggleSwitch(const QString &text, QWidget *parent)
    : QCheckBox(text, parent), motion_(new QVariantAnimation(this)) {
    motion_->setDuration(140);
    motion_->setEasingCurve(QEasingCurve::OutCubic);
    connect(motion_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        position_ = value.toReal();
        update();
    });
    connect(this, &QCheckBox::toggled, this, [this](bool on) {
        motion_->stop();
        if (!isVisible()) {
            position_ = on ? 1 : 0;
            update();
            return;
        }
        motion_->setStartValue(position_);
        motion_->setEndValue(on ? 1.0 : 0.0);
        motion_->start();
    });
}

QSize ToggleSwitch::sizeHint() const {
    const QFontMetrics metrics = fontMetrics();
    const int label = text().isEmpty() ? 0 : metrics.horizontalAdvance(text()) + int(kGap);
    return {label + int(kTrackWidth + 2 * kRingInset), std::max(32, metrics.height() + 8)};
}

bool ToggleSwitch::hitButton(const QPoint &position) const { return rect().contains(position); }

QRectF ToggleSwitch::track() const {
    return {width() - kRingInset - kTrackWidth, (height() - kTrackHeight) / 2, kTrackWidth, kTrackHeight};
}

void ToggleSwitch::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled()) painter.setOpacity(.5);
    const QRectF bar = track();
    const QPalette colors = palette();
    const QRectF label(0, 0, bar.left() - kGap, height());
    painter.setPen(colors.color(QPalette::WindowText));
    painter.drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
                     fontMetrics().elidedText(text(), Qt::ElideRight, int(label.width())));

    QColor off = colors.color(QPalette::WindowText);
    off.setAlphaF(.26f);
    const QColor on = colors.color(QPalette::Highlight);
    const float amount = float(position_);
    const auto mix = [amount](float from, float to) { return from + (to - from) * amount; };
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor::fromRgbF(mix(off.redF(), on.redF()), mix(off.greenF(), on.greenF()),
                                      mix(off.blueF(), on.blueF()), mix(off.alphaF(), on.alphaF())));
    painter.drawRoundedRect(bar, kTrackHeight / 2, kTrackHeight / 2);
    const QPointF knob(bar.left() + kTrackHeight / 2 + position_ * (kTrackWidth - kTrackHeight), bar.center().y());
    painter.setBrush(QColor(0, 0, 0, 56));
    painter.drawEllipse(knob + QPointF(0, 1), kKnob / 2 + .5, kKnob / 2 + .5);
    painter.setBrush(Qt::white);
    painter.drawEllipse(knob, kKnob / 2, kKnob / 2);

    if (hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange)) {
        painter.setOpacity(1);
        painter.setPen(QPen(on, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(bar.adjusted(-2, -2, 2, 2), kTrackHeight / 2 + 2, kTrackHeight / 2 + 2);
    }
}

IconView::IconView(int extent, QWidget *parent) : QWidget(parent), extent_(extent) {
    setFixedSize(extent, extent);
}

void IconView::setIcon(const QIcon &icon) {
    icon_ = icon;
    update();
}

void IconView::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    icon_.paint(&painter, QRect((width() - extent_) / 2, (height() - extent_) / 2, extent_, extent_));
}

QIcon lineIcon(const QString &name, const QColor &color, const QColor &checked, const QColor &disabled) {
    const auto symbol = std::find_if(std::begin(kSymbols), std::end(kSymbols),
                                     [&](const Symbol &candidate) { return name == QLatin1String(candidate.name); });
    if (symbol == std::end(kSymbols)) return {};
    const auto variant = [symbol](const QColor &tint) {
        return tint.isValid() ? strokeSvg(symbol->body, tint) : QByteArray();
    };
    return QIcon(new SvgIconEngine(strokeSvg(symbol->body, color), variant(checked), variant(disabled)));
}

QIcon brandIcon(bool dark) {
    const QString svg = QStringLiteral(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32"><defs><linearGradient id="silver" x1="0" y1="1" x2=".55" y2="0"><stop offset="0" stop-color="%1"/><stop offset=".6" stop-color="%2"/><stop offset="1" stop-color="%3"/></linearGradient></defs><path d="M2.5 29 13 3h6.2L8.8 29Z" fill="url(#silver)"/><path d="M19.2 3 29.5 29h-6.3L16.1 10.7Z" fill="%4"/><path d="M7.6 19.4H25.4l-1.7 4.2H5.9Z" fill="#0A9BFF"/></svg>)svg");
    const QString filled = dark ? svg.arg("#8A949F", "#CDD4DC", "#FFFFFF", "#55626F")
                                : svg.arg("#6E7884", "#B5BEC8", "#EEF1F4", "#2B3440");
    return QIcon(new SvgIconEngine(filled.toUtf8(), {}, {}));
}

} // namespace ashdesk
