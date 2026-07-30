#include "ToggleSwitch.h"
#include <QPainter>
#include <QMouseEvent>

ToggleSwitch::ToggleSwitch(QWidget* parent) : QWidget(parent), m_checked(false)
{
    // 设置滑块的固定大小，与你网页设计稿比例一致
    setFixedSize(52, 26);
    setCursor(Qt::PointingHandCursor);
}

bool ToggleSwitch::isChecked() const { return m_checked; }

void ToggleSwitch::setChecked(bool checked)
{
    if (m_checked != checked) {
        m_checked = checked;
        update(); // 触发重绘
        emit toggled(m_checked);
    }
}

void ToggleSwitch::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing); // 开启抗锯齿，边缘极其圆润

    QRectF rectF = rect().adjusted(1, 1, -1, -1);

    if (m_checked) {
        // [自动状态] - 绿色背景
        p.setBrush(QColor("#1f7f4a"));
        p.setPen(QPen(QColor("#2f9f5a"), 1));
        p.drawRoundedRect(rectF, 12, 12);

        // 画右侧的白色圆球 (Thumb)
        p.setBrush(QColor("#ffffff"));
        p.setPen(Qt::NoPen);
        p.drawEllipse(width() - 24, 3, 20, 20);
    }
    else {
        // [手动状态] - 深灰背景
        p.setBrush(QColor("#2a3542"));
        p.setPen(QPen(QColor("#4a5a6e"), 1));
        p.drawRoundedRect(rectF, 12, 12);

        // 画左侧的浅灰圆球 (Thumb)
        p.setBrush(QColor("#8da3bb"));
        p.setPen(Qt::NoPen);
        p.drawEllipse(4, 3, 20, 20);
    }
}

void ToggleSwitch::mouseReleaseEvent(QMouseEvent* e)
{
    // 鼠标点击松开时，切换状态
    if (e->button() == Qt::LeftButton) {
        setChecked(!m_checked);
    }
}