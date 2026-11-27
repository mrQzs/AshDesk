#include "liquid_glass.h"

#include <gtest/gtest.h>
#include <QTest>
#include <algorithm>
#include <array>
#include <iostream>

// This same test source is also compiled against the historical renderer by
// build_glass_bench.ps1. No font/layout work, shape rebuilds or resize timers
// belong in a comparison of per-position composition.
TEST(GlassBenchmark, FixedPanelMovement) {
    constexpr int kSurfaceWidth = 1120;
    constexpr int kSurfaceHeight = 740;
    constexpr int kPanelWidth = 780;
    constexpr int kPanelHeight = 480;
    constexpr int kInset = 24;
    constexpr int kSettleMs = 180;
    constexpr int kWarmup = 4;
    constexpr size_t kSamples = 60;
    constexpr size_t kP95 = kSamples * 95 / 100;
    constexpr double kNanosecondsPerMillisecond = 1e6;
    ashdesk::GlassSurface surface;
    surface.resize(kSurfaceWidth, kSurfaceHeight);
    surface.setAppearance(false, ashdesk::Material::Liquid, false);
    ashdesk::GlassPanel panel(ashdesk::GlassRole::Action, kInset, &surface);
    panel.setGeometry(kInset, kInset, kPanelWidth, kPanelHeight);
    surface.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&surface));
    QTest::qWait(kSettleMs);
    surface.repaint();
    ASSERT_FALSE(surface.resizing());
    auto &stats = surface.stats();
    const int builds = stats.backdropBuilds;
    std::array<qint64, kSamples> samples{};
    for (size_t i = 0; i < kWarmup + kSamples; ++i) {
        const int before = stats.compositions;
        panel.move(kInset + int((i + 1) % 2), kInset);
        surface.repaint();
        ASSERT_EQ(stats.compositions, before + 1);
        if (i >= kWarmup) {
            samples[i - kWarmup] =
                stats.recentCompositionNanoseconds[size_t(stats.compositions - 1) %
                                                   stats.recentCompositionNanoseconds.size()];
        }
    }
    EXPECT_EQ(stats.backdropBuilds, builds);
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    std::cout << "[ fixed ] panel " << panel.width() << 'x' << panel.height()
              << '@' << surface.devicePixelRatioF()
              << " composition " << double(sorted[kSamples / 2]) / kNanosecondsPerMillisecond
              << '/' << double(sorted[kP95]) / kNanosecondsPerMillisecond << " ms (p50/p95)\n";
    std::cout << "[ fixed samples ns ]";
    for (const qint64 sample : samples) {
        std::cout << ' ' << sample;
    }
    std::cout << '\n';
}
