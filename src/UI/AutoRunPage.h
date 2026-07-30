// AutoRunPage.h
#ifndef AUTORUNPAGE_H
#define AUTORUNPAGE_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>

class AutoRunPage : public QWidget
{
    Q_OBJECT

public:
    explicit AutoRunPage(QWidget* parent = nullptr);

private slots:
    void OnStartClicked();
    void OnResetClicked();
    void OnStopClicked();
    void OnInitClicked();
    void OnEmergencyClicked();

private:
    void SetupUI();

    // UI 组件 (便于后续更新)
    QLabel* m_statusLabel = nullptr;
    QLabel* m_coordPanel = nullptr;
    QWidget* m_logBox = nullptr;
};

#endif // AUTORUNPAGE_H