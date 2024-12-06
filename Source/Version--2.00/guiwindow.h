/*  PIGS-GUI: a configuration utility for the PIGS light gun system.
    Copyright (C) 2024  That One Seong

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef GUIWINDOW_H
#define GUIWINDOW_H

#include <QMainWindow>
#include <QSerialPort>
#include <QGraphicsItem>
#include <QPen>

QT_BEGIN_NAMESPACE
namespace Ui {
class guiWindow;
}
QT_END_NAMESPACE

class guiWindow : public QMainWindow
{
    Q_OBJECT

public slots:
    void on_ledSetupBtn_clicked();  // Declare the slot

public slots:
        void on_lgSetupBtn_clicked();  // Declare the slot


public slots:
        void on_lgTipsBtn_clicked();  // Declare the slot

public:
    guiWindow(QWidget *parent = nullptr);
    ~guiWindow();

    QSerialPort serialPort;

    bool serialActive = false;

private:
    bool isButtonPressed = false; // To track if the button is pressed or released

private:
    void sendSerialCommand(const QString &command);


private slots:
    void on_nunChuckToggle_stateChanged(int arg1);

    void on_comPortSelector_currentIndexChanged(int index);

    void on_confirmButton_clicked();

    void serialPort_readyRead();

    void pinBoxes_activated(int index);

    void irBoxes_activated(int index);

    void runModeBoxes_activated(int index);

    // void on_customPinsEnabled_stateChanged(int arg1);

    void on_rumbleTestBtn_clicked();

    void on_solenoidTestBtn_clicked();

    void on_baudResetBtn_clicked();

    void on_rumbleToggle_stateChanged(int arg1);

    void on_solenoidToggle_stateChanged(int arg1);

    void on_autofireToggle_stateChanged(int arg1);

    void on_holdToPauseToggle_stateChanged(int arg1);

    void on_rumbleIntensityBox_valueChanged(int arg1);

    void on_rumbleLengthBox_valueChanged(int arg1);

    void on_holdToPauseLengthBox_valueChanged(int arg1);

    void on_solenoidNormalIntervalBox_valueChanged(int arg1);

    void on_solenoidFastIntervalBox_valueChanged(int arg1);

    void on_solenoidHoldLengthBox_valueChanged(int arg1);

    void on_autofireWaitFactorBox_valueChanged(int arg1);

    void on_clearEepromBtn_new_clicked();

    void on_testBtn_clicked();

    void selectedProfile_isChecked(bool isChecked);

    void on_calib1Btn_clicked();

    void on_calib2Btn_clicked();

    void on_calib3Btn_clicked();

    void on_calib4Btn_clicked();

    void on_actionAbout_IR_PIGS_triggered();

    void on_pbTransfer_clicked();

    void on_pbRefreshDev_clicked();

    void on_pbReboot_clicked();

private:
    Ui::guiWindow *ui;

    // Used by pinBoxes, matching boardInputs_e
    QStringList valuesNameList = {
        "Unmapped",
        "Trigger",
        "Button A",
        "Button B",
        "Button C",
        "Start",
        "Select",
        "D-Pad Up",
        "D-Pad Down",
        "D-Pad Left",
        "D-Pad Right",
        "External Pedal",
        "Home Button",
        "Pump Action",
        "Rumble Signal",
        "Solenoid Signal",
        "Temp Sensor",
        "Rumble Switch",
        "Solenoid Switch",
        "Autofire Switch",
        "RGB LED Red",
        "RGB LED Green",
        "RGB LED Blue",
        "External NeoPixel",
        "Analog Pin X",
        "Analog Pin Y"
    };

    // List of serial port objects that were found in PortsSearch()
    QList<QSerialPortInfo> serialFoundList;
    // Extracted COM paths, as provided from serialFoundList
    QStringList usbName;

    // Tracks the amount of differences between current config and loaded config.
    // Resets after every call to DiffUpdate()
    uint8_t settingsDiff;

    // Current array of booleans, meant to be used as a bitmask
    bool boolSettings[8];
    // Array of booleans, as loaded from the gun firmware
    bool boolSettings_orig[8];

    // Current table of tunable settings
    uint16_t settingsTable[8];
    // Table of tunables, as loaded from gun firmware
    uint16_t settingsTable_orig[8];

    // because pinBoxes' "->currentIndex" gets updated AFTER calling its activation signal,
    // we need to save its last index to properly compare and prevent duplicate changes,
    // and then update it at the end of the activate signal.
    int pinBoxesOldIndex[30];

    // Uses the same logic as pinBoxesOldIndex, since the irSensor and runMode comboboxes
    // are hooked to a single signal.
    uint8_t irSensOldIndex[4];
    uint8_t runModeOldIndex[4];

    bool testMode = false;

    // Test Mode screen points & colors
    QGraphicsEllipseItem testPointTL;
    QGraphicsEllipseItem testPointTR;
    QGraphicsEllipseItem testPointBL;
    QGraphicsEllipseItem testPointBR;
    QGraphicsEllipseItem testPointMed;
    QGraphicsEllipseItem testPointD;
    QGraphicsPolygonItem testBox;

    QPen testPointTLPen;
    QPen testPointTRPen;
    QPen testPointBLPen;
    QPen testPointBRPen;
    QPen testPointMedPen;
    QPen testPointDPen;

    // ^^^---Values---^^^
    //
    // vvv---Methods---vvv

    void BoxesUpdate();

    void DiffUpdate();

    void PopupWindow(QString errorTitle, QString errorMessage, QString windowTitle, int errorType);

    void PortsSearch();

    void SelectionUpdate(uint8_t newSelection);

    bool SerialInit(int portNum);

    void SerialLoad();

    void SyncSettings();

    void listUsbDevices();
};
#endif // GUIWINDOW_H
