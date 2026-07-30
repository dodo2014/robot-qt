#pragma once

#include <QWidget>

class ManualControlPage : public QWidget
{
    Q_OBJECT

public:
    explicit ManualControlPage(QWidget* parent = nullptr);

private slots:
    void OnGlobalEnable();
    void OnGlobalDisable();
    void OnJogMinus(int axis);
    void OnJogPlus(int axis);
    void OnGoClicked(int axis);
    void OnStopAxis(int axis);

private:
    void SetupUI();
    QWidget* CreateAxisRow(int axis, const QString& label, const QString& typeIcon,
                           double speed, double accel, double decel,
                           const QString& pos, double target, bool isGripper = false, bool isExtrude = false);
};
