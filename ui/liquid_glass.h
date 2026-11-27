#pragma once

#include "ad_glass.h"

#include <QImage>
#include <QTimer>
#include <QWidget>
#include <array>
#include <vector>

namespace ashdesk {

enum class Material { Solid, Frosted, Liquid, Dispersion };
enum class GlassRole { Navigation, Action };

struct GlassStats {
    int backdropBuilds = 0;
    qint64 backdropNanoseconds = 0;
    int compositions = 0;
    std::array<qint64, 128> recentCompositionNanoseconds{};
};

// Owns the app's static backdrop. Glass panels sample it instead of the live desktop,
// so the material is identical on Windows, macOS and Linux and costs nothing while idle.
class GlassSurface final : public QWidget {
    Q_OBJECT
public:
    explicit GlassSurface(QWidget *parent = nullptr);
    void setAppearance(bool dark, Material material, bool highContrast);
    bool dark() const { return dark_; }
    bool highContrast() const { return highContrast_; }
    Material material() const { return material_; }
    // True while a burst of resizes is in progress; glass then stretches cached images.
    bool resizing() const { return resizing_; }
    quint64 version() const { return version_; }
    const QImage &sharp() const { return sharp_; }
    // Quarter resolution: after the frost blur a full-size copy would hold no extra detail.
    const QImage &blurred() const { return blurred_; }
    GlassStats &stats() { return stats_; }
    void ensureBackdrop();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    bool dark_ = false;
    bool highContrast_ = false;
    bool stale_ = true;
    bool resizing_ = false;
    Material material_ = Material::Liquid;
    quint64 version_ = 0;
    QImage sharp_;
    QImage blurred_;
    QTimer settle_;
    GlassStats stats_;
};

class GlassPanel final : public QWidget {
    Q_OBJECT
public:
    GlassPanel(GlassRole role, int radius, QWidget *parent = nullptr);
    void enableFocusRing();
    qsizetype cacheBytes() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct ShapeKey {
        QSize size;
        qreal dpr = 0;
        bool dark = false;
        Material material = Material::Solid;
        bool operator==(const ShapeKey &other) const {
            return size == other.size && qFuzzyCompare(dpr, other.dpr) && dark == other.dark &&
                   material == other.material;
        }
    };

    GlassSurface *surface() const;
    void paintGlass(QPainter &painter, GlassSurface &glass);
    void buildShape(const ShapeKey &key);
    void compose(GlassSurface &surface, QPoint origin);
    void paintSolid(QPainter &painter, bool dark, bool highContrast) const;
    void paintFocusRing(QPainter &painter, bool dark) const;
    void release();

    GlassRole role_;
    int radius_;
    bool focusRing_ = false;
    ShapeKey shape_;
    std::vector<ad_glass_edge> edges_;
    // Shadow pixels under the edges whose cover is below 255, in edge order.
    std::vector<QRgb> rim_;
    QImage overlay_;
    // The drop shadow with the composed glass body on top at bodyOffset_.
    QImage panel_;
    QPoint bodyOffset_;
    quint64 composedVersion_ = 0;
    QPoint composedOrigin_;
};

} // namespace ashdesk
