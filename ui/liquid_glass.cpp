#include "liquid_glass.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLinearGradient>
#include <QPaintEvent>
#include <QPainter>
#include <QRadialGradient>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ashdesk {
namespace {

// Optical constants shared with design/prototype/glass.js.
constexpr float kEta = 1.0f / 1.5f;
constexpr float kEpsilon = 0.08f;
constexpr float kMaxShift = 16.0f;
constexpr float kDispersion = 0.01f;
constexpr float kFrostEnd = 0.85f;
constexpr int kSaturationQ8 = 410; // 1.6
constexpr qreal kBlurSigma = 10.0;
constexpr qreal kShadowSigma = 6.0;
constexpr qreal kShadowOffset = 4.0;
constexpr int kDownscale = 4; // fixed by the ad_glass kernels
constexpr int kOpaque = 255;      // full alpha, and full cover / frost in ad_glass_edge
constexpr int kOffsetOne = 256;   // ad_glass_edge dx/dy are Q8 pixels
constexpr int kRatioOne = 4096;   // ad_glass_edge red/blue are Q12 multiples of the green offset
constexpr float kLightX = -0.55f;
constexpr float kLightY = -0.835f;
const QMargins kShadowMargins(10, 6, 10, 14);

QColor rgba(int red, int green, int blue, qreal alpha) {
    QColor color(red, green, blue);
    color.setAlphaF(float(alpha));
    return color;
}

struct Tokens {
    QColor base0, base1, blobBlue, blobSlate, high, low, blue, blue2, slate, glow;
    QColor background, tintNavigation, tintAction, solidNavigation, solidAction;
    QColor line, strongLine, rim, rim2, sheen, shadow, ring;
    float specular, backSpecular;
};

const Tokens &tokens(bool dark) {
    static const Tokens light{
        QColor(0xEE, 0xF2, 0xF6), QColor(0xDC, 0xE2, 0xEA), rgba(10, 155, 255, .26), rgba(110, 125, 145, .30),
        rgba(255, 255, 255, .95), rgba(160, 172, 190, .55), rgba(10, 155, 255, .70), rgba(10, 155, 255, .40),
        rgba(43, 52, 64, .16), rgba(10, 155, 255, .45),
        QColor(0xE8, 0xEC, 0xF1), rgba(250, 252, 255, .30), rgba(250, 252, 255, .40), QColor(0xF6, 0xF8, 0xFA),
        QColor(0xFF, 0xFF, 0xFF),
        rgba(22, 27, 34, .10), rgba(0, 0, 0, .55), rgba(255, 255, 255, .95), rgba(255, 255, 255, .20),
        rgba(255, 255, 255, .50), rgba(15, 25, 40, .16), QColor(0x0A, 0x74, 0xDA), .60f, .30f};
    static const Tokens night{
        QColor(0x11, 0x17, 0x20), QColor(0x0A, 0x0E, 0x13), rgba(10, 155, 255, .24), rgba(120, 135, 160, .16),
        rgba(215, 225, 238, .26), rgba(140, 150, 165, .06), rgba(10, 155, 255, .80), rgba(10, 155, 255, .45),
        rgba(120, 135, 155, .14), rgba(10, 155, 255, .45),
        QColor(0x0E, 0x12, 0x17), rgba(30, 36, 46, .38), rgba(30, 36, 46, .46), QColor(0x14, 0x19, 0x20),
        QColor(0x1B, 0x21, 0x29),
        rgba(233, 237, 242, .09), rgba(255, 255, 255, .60), rgba(255, 255, 255, .32), rgba(255, 255, 255, .05),
        rgba(255, 255, 255, .14), rgba(0, 0, 0, .45), QColor(0x6B, 0xBC, 0xFF), .30f, .14f};
    return dark ? night : light;
}

QColor clear(QColor color) {
    color.setAlpha(0);
    return color;
}

// CSS linear-gradient(<angle>): the gradient line crosses the centre and reaches the far corners.
QLinearGradient cssLinear(const QRectF &box, qreal degrees) {
    const qreal angle = qDegreesToRadians(degrees);
    const QPointF direction(std::sin(angle), -std::cos(angle));
    const qreal half = (std::abs(box.width() * direction.x()) + std::abs(box.height() * direction.y())) / 2;
    return QLinearGradient(box.center() - direction * half, box.center() + direction * half);
}

void paintRadial(QPainter &painter, QPointF centre, QSizeF radius, const QColor &color, qreal stop) {
    QRadialGradient gradient(QPointF(0, 0), 1);
    gradient.setColorAt(0, color);
    gradient.setColorAt(stop, clear(color));
    painter.save();
    painter.translate(centre);
    painter.scale(radius.width(), radius.height());
    painter.fillRect(QRectF(-1, -1, 2, 2), gradient);
    painter.restore();
}

// Approximates CSS box-shadow: 0 0 40px around a pill whose ends lie outside the window.
void paintGlow(QPainter &painter, const QRectF &pill, const QColor &color) {
    constexpr qreal spread = 40;
    const QRectF area = pill.adjusted(-spread, 0, spread, 0);
    QLinearGradient glow(area.topLeft(), area.topRight());
    const qreal falloff[][2] = {{0, .5}, {10, .31}, {20, .16}, {30, .067}, {40, 0}};
    for (const auto &[distance, strength] : falloff) {
        QColor stop = color;
        stop.setAlphaF(float(color.alphaF() * strength));
        glow.setColorAt((spread - distance) / area.width(), stop);
        glow.setColorAt(1 - (spread - distance) / area.width(), stop);
    }
    painter.fillRect(area, glow);
}

void paintBackdrop(QPainter &painter, const QSizeF &size, const Tokens &t) {
    const qreal w = size.width(), h = size.height();
    const QRectF box(QPointF(0, 0), size);
    QLinearGradient base = cssLinear(box, 160);
    base.setColorAt(0, t.base0);
    base.setColorAt(1, t.base1);
    painter.fillRect(box, base);
    paintRadial(painter, {.94 * w, .92 * h}, {.50 * w, .60 * h}, t.blobSlate, .7);
    paintRadial(painter, {.10 * w, .06 * h}, {.55 * w, .50 * h}, t.blobBlue, .7);
    struct Band {
        qreal left, width;
        QColor top, bottom;
        bool glow;
    };
    const Band bands[] = {{-.02, 54, t.high, t.low, false}, {.34, 130, t.high, t.low, false},
                          {.50, 22, t.blue, t.blue, true}, {.70, 96, t.slate, t.slate, false},
                          {.87, 14, t.blue2, t.blue2, false}};
    painter.setPen(Qt::NoPen);
    for (const Band &band : bands) {
        const QRectF pill(-band.width / 2, -.8 * h, band.width, 1.6 * h);
        painter.save();
        painter.translate(band.left * w + band.width / 2, .5 * h);
        painter.rotate(24);
        if (band.glow) paintGlow(painter, pill, t.glow);
        QLinearGradient fill(pill.topLeft(), pill.bottomLeft());
        fill.setColorAt(0, band.top);
        fill.setColorAt(1, band.bottom);
        painter.setBrush(fill);
        painter.drawRoundedRect(pill, band.width / 2, band.width / 2);
        painter.restore();
    }
}

int boxRadius(qreal sigma) {
    // Three box passes approximate a Gaussian of this sigma.
    return std::max(1, qRound((std::sqrt(4 * sigma * sigma + 1) - 1) / 2));
}

ad_glass_view viewOf(const QImage &image) {
    return {reinterpret_cast<const uint32_t *>(image.constBits()), image.bytesPerLine() / qsizetype(sizeof(QRgb)),
            image.width(), image.height()};
}

ad_glass_image imageOf(QImage &image) {
    return {reinterpret_cast<uint32_t *>(image.bits()), image.bytesPerLine() / qsizetype(sizeof(QRgb)), image.width(),
            image.height()};
}

void boxBlur(QImage &image, int radius) {
    std::vector<uint32_t> scratch((ad_glass_blur_scratch(image.width(), image.height()) + 3) / 4);
    ad_glass_box_blur(imageOf(image), radius, 3, scratch.data());
}

// Fills `target` with the full-resolution pixels of `quarter` starting at device position `origin`.
void upsample(const QImage &quarter, QPoint origin, ad_glass_image target) {
    std::vector<int16_t> scratch(ad_glass_upsample_scratch(target.width));
    ad_glass_upsample4(viewOf(quarter), origin.x(), origin.y(), target, scratch.data());
}

QImage frost(const QImage &sharp, qreal dpr) {
    QImage quarter(std::max(1, sharp.width() / kDownscale), std::max(1, sharp.height() / kDownscale),
                   QImage::Format_RGB32);
    ad_glass_downsample4(viewOf(sharp), imageOf(quarter));
    boxBlur(quarter, boxRadius(kBlurSigma * dpr / kDownscale));
    ad_glass_saturate(imageOf(quarter), kSaturationQ8);
    return quarter;
}

QImage dropShadow(const QSize &device, const QRectF &body, qreal radius, qreal dpr, const QColor &color) {
    QImage small((device.width() + kDownscale - 1) / kDownscale, (device.height() + kDownscale - 1) / kDownscale,
                 QImage::Format_ARGB32_Premultiplied);
    small.fill(Qt::transparent);
    {
        QPainter painter(&small);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(dpr / kDownscale, dpr / kDownscale);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(body.translated(0, kShadowOffset), radius, radius);
    }
    boxBlur(small, boxRadius(kShadowSigma * dpr / kDownscale));
    QImage shadow(device, QImage::Format_ARGB32_Premultiplied);
    upsample(small, QPoint(0, 0), imageOf(shadow));
    shadow.setDevicePixelRatio(dpr);
    {
        QPainter painter(&shadow);
        painter.setRenderHint(QPainter::Antialiasing);
        QColor contact = color;
        contact.setAlphaF(float(color.alphaF() * .6));
        painter.setPen(QPen(contact, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(body.translated(0, .5).adjusted(-.5, -.5, .5, .5), radius + .5, radius + .5);
    }
    return shadow;
}

float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}

// Lateral travel, along the outward normal, of a view ray refracted at the glass surface and
// stopped at the backdrop plane `depth` below it. Negative values pull samples inward.
float travel(float eta, float nz, float lateral, float depth) {
    const float transmitted = std::sqrt(std::max(0.f, 1 - eta * eta * (1 - nz * nz)));
    const float along = eta * nz - transmitted;
    const float down = -eta + along * nz;
    return along * lateral * depth / -down;
}

QRgb lighten(QRgb below, float alpha) {
    const auto mix = [&](int value) {
        return std::clamp(qRound(kOpaque * alpha + float(value) * (1 - alpha)), 0, kOpaque);
    };
    return qRgba(mix(qRed(below)), mix(qGreen(below)), mix(qBlue(below)), mix(qAlpha(below)));
}

// ad_glass_edge stores offsets and ratios in fixed point with `one` steps per unit.
qint16 fixed(float value, int one) {
    constexpr int limit = std::numeric_limits<qint16>::max();
    return qint16(std::clamp(qRound(value * float(one)), -limit, limit));
}

quint8 unit(float value) { return quint8(qRound(value * kOpaque)); }

} // namespace

GlassSurface::GlassSurface(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    settle_.setSingleShot(true);
    settle_.setInterval(120);
    connect(&settle_, &QTimer::timeout, this, [this] {
        if (std::exchange(resizing_, false)) update();
    });
}

void GlassSurface::setAppearance(bool dark, Material material, bool highContrast) {
    if (highContrast) material = Material::Solid;
    if (dark == dark_ && material == material_ && highContrast == highContrast_) return;
    dark_ = dark;
    material_ = material;
    highContrast_ = highContrast;
    stale_ = true;
    if (material_ == Material::Solid) {
        sharp_ = {};
        blurred_ = {};
    }
    update();
}

void GlassSurface::ensureBackdrop() {
    if (material_ == Material::Solid || (resizing_ && !sharp_.isNull() && !stale_)) return;
    const qreal dpr = devicePixelRatioF();
    const QSize device(std::max(1, qCeil(width() * dpr)), std::max(1, qCeil(height() * dpr)));
    if (!stale_ && sharp_.size() == device && qFuzzyCompare(sharp_.devicePixelRatio(), dpr)) return;
    QElapsedTimer timer;
    timer.start();
    sharp_ = QImage(device, QImage::Format_RGB32);
    sharp_.setDevicePixelRatio(dpr);
    {
        QPainter painter(&sharp_);
        painter.setRenderHint(QPainter::Antialiasing);
        paintBackdrop(painter, QSizeF(device) / dpr, tokens(dark_));
    }
    blurred_ = frost(sharp_, dpr);
    stale_ = false;
    ++version_;
    ++stats_.backdropBuilds;
    stats_.backdropNanoseconds = timer.nsecsElapsed();
}

void GlassSurface::paintEvent(QPaintEvent *event) {
    QPainter painter(this);
    if (material_ == Material::Solid) {
        painter.fillRect(event->rect(), tokens(dark_).background);
        return;
    }
    ensureBackdrop();
    if (sharp_.size() == QSize(qCeil(width() * devicePixelRatioF()), qCeil(height() * devicePixelRatioF())))
        painter.drawImage(QPoint(0, 0), sharp_);
    else
        painter.drawImage(rect(), sharp_);
}

// The first resize of a burst (maximise, snap) rebuilds at once; later ones within the
// settle interval are interactive drags and only stretch the cached glass until it settles.
void GlassSurface::resizeEvent(QResizeEvent *event) {
    resizing_ = settle_.isActive();
    settle_.start();
    QWidget::resizeEvent(event);
}

GlassPanel::GlassPanel(GlassRole role, int radius, QWidget *parent)
    : QWidget(parent), role_(role), radius_(radius) {
    setContentsMargins(kShadowMargins);
}

void GlassPanel::enableFocusRing() {
    if (focusRing_) return;
    focusRing_ = true;
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *previous, QWidget *current) {
        if (isAncestorOf(previous) || isAncestorOf(current)) update();
    });
}

qsizetype GlassPanel::cacheBytes() const {
    return overlay_.sizeInBytes() + panel_.sizeInBytes() + qsizetype(edges_.capacity() * sizeof(ad_glass_edge)) +
        qsizetype(rim_.capacity() * sizeof(QRgb));
}

void GlassPanel::paintEvent(QPaintEvent *) {
    GlassSurface *glass = surface();
    const bool dark = glass && glass->dark();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!glass || glass->material() == Material::Solid) {
        if (!shape_.size.isEmpty()) release();
        paintSolid(painter, dark, glass && glass->highContrast());
    } else {
        glass->ensureBackdrop();
        paintGlass(painter, *glass);
    }
    if (focusRing_ && isAncestorOf(QApplication::focusWidget())) paintFocusRing(painter, dark);
}

void GlassPanel::hideEvent(QHideEvent *event) {
    release();
    QWidget::hideEvent(event);
}

void GlassPanel::paintGlass(QPainter &painter, GlassSurface &glass) {
    const qreal dpr = devicePixelRatioF();
    const QRect body = contentsRect();
    const ShapeKey key{QSize(qCeil(body.width() * dpr), qCeil(body.height() * dpr)), dpr, glass.dark(), glass.material()};
    if (key.size.isEmpty()) return;
    // Mid-drag the backdrop is not rebuilt, so the last composition is still the right material.
    if (glass.resizing() && !panel_.isNull() && composedVersion_ == glass.version()) {
        if (!(key == shape_)) {
            painter.drawImage(QRectF(rect()), panel_);
            return;
        }
    } else {
        if (!(key == shape_)) buildShape(key);
        const QPoint origin = (QPointF(mapTo(&glass, body.topLeft())) * dpr).toPoint();
        if (composedVersion_ != glass.version() || composedOrigin_ != origin) compose(glass, origin);
    }
    // The body lands on the device pixel it rounds to on its own; the soft shadow absorbs the
    // sub-pixel difference, so the edge matches wherever the panel sits.
    const QPointF window = QPointF(mapTo(this->window(), QPoint(0, 0)));
    const QPoint bodyPixel = ((window + QPointF(body.topLeft())) * dpr).toPoint();
    painter.drawImage(QPointF(bodyPixel - bodyOffset_) / dpr - window, panel_);
}

GlassSurface *GlassPanel::surface() const {
    for (QWidget *widget = parentWidget(); widget; widget = widget->parentWidget())
        if (auto *glass = qobject_cast<GlassSurface *>(widget)) return glass;
    return nullptr;
}

// Everything that depends only on size, theme and material: tint, sheen, rim, specular
// glints, per-edge refraction offsets and the drop shadow.
void GlassPanel::buildShape(const ShapeKey &key) {
    shape_ = key;
    composedVersion_ = 0;
    const Tokens &palette = tokens(key.dark);
    const qreal dpr = key.dpr;
    const int w = key.size.width(), h = key.size.height();
    const QRectF box(QPointF(0, 0), QSizeF(key.size) / dpr);
    const qreal radius = std::min<qreal>(radius_, std::min(box.width(), box.height()) / 2);
    overlay_ = QImage(key.size, QImage::Format_ARGB32_Premultiplied);
    overlay_.fill(Qt::transparent);
    overlay_.setDevicePixelRatio(dpr);
    {
        QPainter painter(&overlay_);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(box, role_ == GlassRole::Navigation ? palette.tintNavigation : palette.tintAction);
        paintRadial(painter, {.18 * box.width(), -.25 * box.height()}, {1.4 * box.width(), .9 * box.height()},
                    palette.sheen, .55);
        QLinearGradient rim = cssLinear(box, 160);
        rim.setColorAt(0, palette.rim);
        rim.setColorAt(.38, palette.rim2);
        rim.setColorAt(.55, clear(palette.rim2));
        rim.setColorAt(.85, palette.rim2);
        rim.setColorAt(1, palette.rim);
        painter.setPen(QPen(QBrush(rim), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(box.adjusted(.5, .5, -.5, -.5), radius - .5, radius - .5);
    }
    const QRectF bodyRect = QRectF(contentsRect());
    panel_ = dropShadow(QSize(qCeil(width() * dpr), qCeil(height() * dpr)), bodyRect, radius, dpr, palette.shadow);
    bodyOffset_ = (bodyRect.topLeft() * dpr).toPoint();

    const bool refract = key.material != Material::Frosted;
    const float scale = float(dpr);
    const float hx = float(w) / 2, hy = float(h) / 2;
    const float r = float(radius) * scale;
    const float roleBevel = role_ == GlassRole::Navigation ? 14.f : 12.f;
    const float bevel = std::min({roleBevel * scale, hx, hy});
    const float base = (role_ == GlassRole::Navigation ? 8.f : 7.f) * scale;
    const float limit = kMaxShift * scale;
    const float core = std::max(r, bevel);
    edges_.clear();
    for (int y = 0; y < h; ++y) {
        auto *overlay = reinterpret_cast<QRgb *>(overlay_.scanLine(y));
        const float py = float(y) + .5f - hy;
        const float qy = std::abs(py) - (hy - r);
        const bool middleRow = std::abs(py) <= hy - core;
        for (int x = 0; x < w; ++x) {
            const float px = float(x) + .5f - hx;
            if (middleRow && std::abs(px) <= hx - core) {
                x = w - 1 - x;
                continue;
            }
            const float qx = std::abs(px) - (hx - r);
            const float outside = std::hypot(std::max(qx, 0.f), std::max(qy, 0.f));
            const float phi = outside + std::min(std::max(qx, qy), 0.f) - r;
            if (phi <= -bevel) continue;
            float gx = 0, gy = 0;
            if (qx > 0 && qy > 0) {
                gx = qx / outside;
                gy = qy / outside;
            } else if (qx > qy) {
                gx = 1;
            } else {
                gy = 1;
            }
            gx = std::copysign(gx, px);
            gy = std::copysign(gy, py);
            ad_glass_edge edge{};
            edge.x = qint16(x);
            edge.y = qint16(y);
            edge.red = edge.blue = qint16(kRatioOne);
            edge.frost = kOpaque;
            edge.cover = unit(std::clamp(.5f - phi, 0.f, 1.f));
            const float t = std::clamp(-phi / bevel, 0.f, 1.f);
            if (refract && edge.cover > 0) {
                const float slope = 1 - t;
                const float lift = std::sqrt(2 * t - t * t + kEpsilon * kEpsilon);
                const float norm = std::sqrt(slope * slope + lift * lift);
                const float lateral = slope / norm, nz = lift / norm;
                const float depth = base + bevel * (lift - kEpsilon);
                const float green = travel(kEta, nz, lateral, depth);
                const float shift = std::clamp(green, -limit, limit);
                edge.dx = fixed(gx * shift, kOffsetOne);
                edge.dy = fixed(gy * shift, kOffsetOne);
                if (key.material == Material::Dispersion && std::abs(green) > 1e-4f) {
                    edge.red = fixed(travel(kEta * (1 + kDispersion), nz, lateral, depth) / green, kRatioOne);
                    edge.blue = fixed(travel(kEta * (1 - kDispersion), nz, lateral, depth) / green, kRatioOne);
                }
                edge.frost = unit(smoothstep(0, kFrostEnd, t));
                const float facing = gx * kLightX + gy * kLightY;
                const float front = std::max(facing, 0.f), back = std::max(-facing, 0.f);
                const float glint = slope * slope * std::sqrt(slope) *
                    (palette.specular * front * front + palette.backSpecular * back * back);
                if (glint > 0.004f) overlay[x] = lighten(overlay[x], std::min(glint, 1.f));
            }
            // An opaque, unrefracted, fully frosted pixel already holds the blurred backdrop.
            if (edge.dx != 0 || edge.dy != 0 || edge.frost < kOpaque || edge.cover < kOpaque) edges_.push_back(edge);
        }
    }
    edges_.shrink_to_fit();
    rim_.clear();
    for (const ad_glass_edge &edge : edges_) {
        if (edge.cover == kOpaque) continue;
        const auto *row = reinterpret_cast<const QRgb *>(panel_.constScanLine(bodyOffset_.y() + edge.y));
        rim_.push_back(row[bodyOffset_.x() + edge.x]);
    }
    rim_.shrink_to_fit();
}

// Per-position work: refract the cached backdrop through the cached edge map, then put the
// sheen on top and the shadow back under the antialiased rim.
void GlassPanel::compose(GlassSurface &glass, QPoint origin) {
    QElapsedTimer timer;
    timer.start();
    const int w = shape_.size.width(), h = shape_.size.height();
    const qsizetype stride = panel_.bytesPerLine() / qsizetype(sizeof(QRgb));
    QRgb *bits = reinterpret_cast<QRgb *>(panel_.bits()) + bodyOffset_.y() * stride + bodyOffset_.x();
    const ad_glass_image body{bits, stride, w, h};
    upsample(glass.blurred(), origin, body);
    const ad_glass_compose_params params{viewOf(glass.sharp()), viewOf(glass.blurred()), origin.x(), origin.y(),
                                         kSaturationQ8, shape_.material == Material::Dispersion ? 1 : 0};
    ad_glass_compose_edges(&params, edges_.data(), edges_.size(), body);
    {
        QImage target(reinterpret_cast<uchar *>(bits), w, h, panel_.bytesPerLine(), QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&target);
        painter.setCompositionMode(QPainter::CompositionMode_SourceAtop);
        painter.drawImage(QRect(0, 0, w, h), overlay_, overlay_.rect());
    }
    auto shadow = rim_.cbegin();
    for (const ad_glass_edge &edge : edges_) {
        if (edge.cover == kOpaque) continue;
        QRgb &pixel = bits[edge.y * stride + edge.x];
        const int keep = kOpaque - qAlpha(pixel);
        const auto under = [&](int above, int below) { return above + (below * keep + kOpaque / 2) / kOpaque; };
        pixel = qRgba(under(qRed(pixel), qRed(*shadow)), under(qGreen(pixel), qGreen(*shadow)),
                      under(qBlue(pixel), qBlue(*shadow)), under(qAlpha(pixel), qAlpha(*shadow)));
        ++shadow;
    }
    composedVersion_ = glass.version();
    composedOrigin_ = origin;
    GlassStats &stats = glass.stats();
    stats.recentCompositionNanoseconds[size_t(stats.compositions) % stats.recentCompositionNanoseconds.size()] =
        timer.nsecsElapsed();
    ++stats.compositions;
}

void GlassPanel::paintSolid(QPainter &painter, bool dark, bool highContrast) const {
    const Tokens &t = tokens(dark);
    painter.setPen(QPen(highContrast ? t.strongLine : t.line, 1));
    painter.setBrush(role_ == GlassRole::Navigation ? t.solidNavigation : t.solidAction);
    painter.drawRoundedRect(QRectF(contentsRect()).adjusted(.5, .5, -.5, -.5), radius_, radius_);
}

void GlassPanel::paintFocusRing(QPainter &painter, bool dark) const {
    painter.setPen(QPen(tokens(dark).ring, 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(contentsRect()).adjusted(-1, -1, 1, 1), radius_ + 1, radius_ + 1);
}

void GlassPanel::release() {
    shape_ = {};
    edges_.clear();
    edges_.shrink_to_fit();
    rim_.clear();
    rim_.shrink_to_fit();
    overlay_ = {};
    panel_ = {};
}

} // namespace ashdesk
