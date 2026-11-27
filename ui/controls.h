#pragma once

#include <QCheckBox>
#include <QIcon>
#include <QLayout>

class QVariantAnimation;

namespace ashdesk {

// 从左到右排，一行放不下就折到下一行；高度随宽度变（窄窗口里设备权限的勾选项靠它折行，不撑出水平滚动）
class FlowLayout final : public QLayout {
public:
    explicit FlowLayout(int spacing);
    ~FlowLayout() override;
    void addItem(QLayoutItem *item) override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QLayoutItem *takeAt(int index) override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect &rect) override;

private:
    // 按 rect 的宽度排一遍，返回用掉的高度；apply 为 false 时只算不摆
    int arrange(const QRect &rect, bool apply) const;

    QList<QLayoutItem *> items_;
};

// Settings-row switch: text on the left, track on the right edge. Stays a QCheckBox so
// accessibility, keyboard toggling and `toggled` keep their standard behaviour.
class ToggleSwitch final : public QCheckBox {
    Q_OBJECT
public:
    explicit ToggleSwitch(const QString &text, QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    bool hitButton(const QPoint &position) const override;
    void paintEvent(QPaintEvent *event) override;

private:
    QRectF track() const;

    QVariantAnimation *motion_;
    qreal position_ = 0;
};

// Paints an icon at the current device pixel ratio, so it stays crisp across screens.
class IconView final : public QWidget {
    Q_OBJECT
public:
    explicit IconView(int extent, QWidget *parent = nullptr);
    void setIcon(const QIcon &icon);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QIcon icon_;
    int extent_;
};

// Stroke icons from design/prototype/index.html. `checked` colours QIcon::On; `disabled` replaces the default fade.
QIcon lineIcon(const QString &name, const QColor &color, const QColor &checked = {}, const QColor &disabled = {});
QIcon brandIcon(bool dark);

} // namespace ashdesk
