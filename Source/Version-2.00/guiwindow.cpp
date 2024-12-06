/*  P.I.G.S-GUI: a configuration utility for the P.I.G.S light gun system.
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

#include "guiwindow.h"
#include "constants.h"
#include "qlineedit.h"
#include "ui_guiwindow.h"
#include "ui_about.h"
#include <QGraphicsScene>
#include <QMessageBox>
#include <QRadioButton>
#include <QSvgRenderer>
#include <QSvgWidget>
#include <QSerialPortInfo>
#include <QtDebug>
#include <QProgressBar>
#include <QProcess>
#include <QStorageInfo>
#include <QThread>
#include <QCoreApplication>
#include <QMessageBox>


// Currently loaded board object
boardInfo_s board;

// Currently loaded board's TinyUSB identifier info
tinyUSBtable_s tinyUSBtable;
// TinyUSB ident, as loaded from the board
tinyUSBtable_s tinyUSBtable_orig;

// Current calibration profiles
QVector<profilesTable_s> profilesTable(4);
// Calibration profiles, as loaded from the board
QVector<profilesTable_s> profilesTable_orig(4);

// Indexed array map of the current physical layout of the board.
// Key = pin number, Value = pin function
// Values: -2 = N/A, -1 = reserved, 0 = available, unused
QMap<uint8_t, int8_t> currentPins;

#define INPUTS_COUNT 25
// Map of what inputs are put where,
// Key = button/output, Value = pin number occupying, if any.
// Value of -1 means unmapped.
// Key order based on boardInputs_e, minus 1
// Map functions used in deduplication
QMap<uint8_t, int8_t> inputsMap;
// Inputs map, as loaded from the board
QMap<uint8_t, int8_t> inputsMap_orig;

// ^^^-----Typedefs up there:----^^^
//
// vvv---UI Objects down here:---vvv

// Guess I'll have to do the dynamic layout spawning/destroying stuff to make things work.
// How else do I hide things lol?
// QVBoxLayout *PinsCenter;
// QGridLayout *PinsCenterSub;
// QGridLayout *PinsLeft;
// QGridLayout *PinsRight;

QComboBox *pinBoxes[30];
QLabel *pinLabel[30];
QWidget *padding[30];

QRadioButton *selectedProfile[4];
QLabel *xScale[4];
QLabel *yScale[4];
QLabel *xCenter[4];
QLabel *yCenter[4];
QComboBox *irSens[4];
QComboBox *runMode[4];
QSvgWidget *centerPic;

QGraphicsScene *testScene;

//
// ^^^-------GLOBAL VARS UP THERE----------^^^
//
// vvv-------GUI METHODS DOWN HERE---------vvv

void guiWindow::PortsSearch()
{
    serialFoundList = QSerialPortInfo::availablePorts();

    // Always keep "Pick LightGun Here" as the first entry
    QString placeholderText = "Pick LightGun Here";
    if (ui->comPortSelector->itemText(0) != placeholderText) {
        ui->comPortSelector->clear();
        ui->comPortSelector->addItem(placeholderText);
    }

    // Clear all entries except the placeholder
    while (ui->comPortSelector->count() > 1) {
        ui->comPortSelector->removeItem(1);
    }

    if (serialFoundList.isEmpty()) {
        ui->comPortSelector->addItem("Plug in LightGun");
        PopupWindow("No devices detected!",
                    "No serial ports are available. Is the microcontroller board connected and powered?",
                    "ERROR", 4);
        return;
    }

    QMap<QPair<int, int>, QString> piggieMap = {
        {{0x0321, 0x0420}, "Piggie 1"},
        {{0x0322, 0x0421}, "Piggie 2"},
        {{0x0323, 0x0422}, "Piggie 3"},
        {{0x0324, 0x0423}, "Piggie 4"}
    };

    bool lightgunFound = false;
    for (const QSerialPortInfo &portInfo : serialFoundList) {
        QPair<int, int> vidPid = {portInfo.vendorIdentifier(), portInfo.productIdentifier()};

        // Check if the VID/PID matches known devices
        if (piggieMap.contains(vidPid)) {
            QString displayName = piggieMap.value(vidPid); // Friendly name (e.g., "Piggie 1")

            // Clean up the port system location
            QString cleanedLocation = portInfo.systemLocation();
            cleanedLocation.remove("\\\\.\\"); // Remove unwanted prefixes

            // Add entry to the dropdown: "Friendly Name (Cleaned Location)"
            ui->comPortSelector->addItem(displayName + " (" + cleanedLocation + ")");
            qDebug() << "Added to dropdown:" << displayName << "@" << cleanedLocation;

            lightgunFound = true;
        }
    }

    if (!lightgunFound) {
        ui->comPortSelector->addItem("Plug in LightGun");
        PopupWindow("No P.I.G.S devices detected!",
                    "No recognized P.I.G.S devices were found. Check the connection and ensure compatible firmware is installed.",
                    "WARNING", 2);
    }
}


guiWindow::guiWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::guiWindow)
{
    ui->setupUi(this);
    on_pbRefreshDev_clicked();

#ifdef Q_OS_UNIX
    if(qEnvironmentVariable("USER") != "root") {
        QProcess *externalProg = new QProcess;
        QStringList args;
        externalProg->start("/usr/bin/groups", args);
        externalProg->waitForFinished();
        if(!externalProg->readAllStandardOutput().contains("dialout")) {
            PopupWindow("User doesn't have serial permissions!", QString("Currently, your user is not allowed to have access to serial devices.\n\nTo add yourself to the right group, run this command in a terminal and then re-login to your session: \n\nsudo usermod -aG dialout %1").arg(qEnvironmentVariable("USER")), "Permission error", 2);
            exit(0);
        }
    } else {
        PopupWindow("Running as root is not allowed!", "Please run P.I.G.S-GUI as a normal user.", "ERROR", 4);
        exit(2);
    }
#endif

    connect(&serialPort, &QSerialPort::readyRead, this, &guiWindow::serialPort_readyRead);

    // just to be sure, init the inputsMap hashes
    for(uint8_t i = 0; i < INPUTS_COUNT; i++) {
        inputsMap[i] = -1;
        inputsMap_orig[i] = -1;
    }

    // sending all these children to die upon comPortSelector->on_currentIndexChanged
    // (which gets fired immediately after ui->comPortSelector->addItems).
    // PinsCenter = new QVBoxLayout();
    // PinsCenterSub = new QGridLayout();
    // PinsLeft = new QGridLayout();
    // PinsRight = new QGridLayout();

    // ui->PinsTopHalf->addLayout(PinsLeft);
    // ui->PinsTopHalf->addLayout(PinsCenter);
    // ui->PinsTopHalf->addLayout(PinsRight);

    for(uint8_t i = 0; i < 30; i++) {
        pinBoxes[i] = new QComboBox();
        connect(pinBoxes[i], SIGNAL(activated(int)), this, SLOT(pinBoxes_activated(int)));
        pinLabel[i] = new QLabel();
        pinLabel[i]->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        padding[i] = new QWidget();
        padding[i]->setMinimumHeight(25);
    }

    // These can actually stay, tho.
    for(uint8_t i = 0; i < 4; i++) {
        selectedProfile[i] = new QRadioButton(QString("%1.").arg(i+1));
        connect(selectedProfile[i], SIGNAL(toggled(bool)), this, SLOT(selectedProfile_isChecked(bool)));
        xScale[i] = new QLabel("0");
        yScale[i] = new QLabel("0");
        xCenter[i] = new QLabel("0");
        yCenter[i] = new QLabel("0");
        irSens[i] = new QComboBox();
        runMode[i] = new QComboBox();
        xScale[i]->setAlignment(Qt::AlignHCenter);
        yScale[i]->setAlignment(Qt::AlignHCenter);
        xCenter[i]->setAlignment(Qt::AlignHCenter);
        yCenter[i]->setAlignment(Qt::AlignHCenter);
        irSens[i]->addItem("Default");
        irSens[i]->addItem("Higher");
        irSens[i]->addItem("Highest");
        connect(irSens[i], SIGNAL(activated(int)), this, SLOT(irBoxes_activated(int)));
        runMode[i]->addItem("Normal");
        runMode[i]->addItem("1-Frame Avg");
        runMode[i]->addItem("2-Frame Avg");
        connect(runMode[i], SIGNAL(activated(int)), this, SLOT(runModeBoxes_activated(int)));
        ui->profilesArea->addWidget(selectedProfile[i], i+1, 0, 1, 1);
        ui->profilesArea->addWidget(xScale[i], i+1, 1, 1, 1);
        ui->profilesArea->addWidget(yScale[i], i+1, 3, 1, 1);
        ui->profilesArea->addWidget(xCenter[i], i+1, 5, 1, 1);
        ui->profilesArea->addWidget(yCenter[i], i+1, 7, 1, 1);
        ui->profilesArea->addWidget(irSens[i], i+1, 9, 1, 1);
        ui->profilesArea->addWidget(runMode[i], i+1, 11, 1, 1);
    }

    // Setup Test Mode screen colors
    testPointTLPen.setColor(Qt::green);
    testPointTRPen.setColor(Qt::green);
    testPointBLPen.setColor(Qt::blue);
    testPointBRPen.setColor(Qt::blue);
    testPointMedPen.setColor(Qt::gray);
    testPointDPen.setColor(Qt::red);
    testPointTLPen.setWidth(3);
    testPointTRPen.setWidth(3);
    testPointBLPen.setWidth(3);
    testPointBRPen.setWidth(3);
    testPointMedPen.setWidth(3);
    testPointDPen.setWidth(3);
    testPointTL.setPen(testPointTLPen);
    testPointTR.setPen(testPointTRPen);
    testPointBL.setPen(testPointBLPen);
    testPointBR.setPen(testPointBRPen);
    testPointMed.setPen(testPointMedPen);
    testPointD.setPen(testPointDPen);

    // Actually setup the Test Mode scene
    testScene = new QGraphicsScene();
    testScene->setSceneRect(0, 0, 1024, 768);
    testScene->setBackgroundBrush(Qt::darkGray);
    ui->testView->setScene(testScene);
    testScene->addItem(&testBox);
    testScene->addItem(&testPointTL);
    testScene->addItem(&testPointTR);
    testScene->addItem(&testPointBL);
    testScene->addItem(&testPointBR);
    testScene->addItem(&testPointMed);
    testScene->addItem(&testPointD);
    // TODO: is there a way of dynamically scaling QGraphicsViews?
    ui->testView->scale(0.5, 0.5);

    // Finally get to the thing!
    statusBar()->showMessage("Welcome to P.I.G.S-GUI!", 3000);
    PortsSearch();
    // TODO: what's a good validator to only accept character values within the range of an unsigned char?
    //ui->productNameInput->setValidator(new QRegExpValidator(QRegExp("[A-Za-z0-9_]+"), this));
    ui->comPortSelector->addItems(usbName);
}

guiWindow::~guiWindow()
{
    if(serialPort.isOpen()) {
        statusBar()->showMessage("Sending undock request to board...");
        serialPort.write("XE");
        serialPort.waitForBytesWritten(2000);
        serialPort.waitForReadyRead(2000);
        serialPort.close();
    }
    delete ui;
}

void guiWindow::on_ledSetupBtn_clicked()
{
    serialPort.write("LED_SETUP_CMD"); // Command for LED setup (replace with your actual command)
        QLabel *imageLabel = new QLabel(this); // Create QLabel to hold the image
        QPixmap pixmap(":/images/setup/LED-Setup.png"); // Load the image for LED setup

        // Scale the image to the size of the window or screen
        pixmap = pixmap.scaled(this->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);

        imageLabel->setPixmap(pixmap); // Set the scaled image to the label
        imageLabel->setAlignment(Qt::AlignCenter); // Center the image in the label
        imageLabel->setGeometry(0, 0, this->width(), this->height()); // Set label to cover the entire window

        // Create the close button
        QPushButton *closeButton = new QPushButton("Close", this);
        closeButton->setGeometry(this->width() - 100, this->height() - 50, 80, 30); // Position the button in the bottom right corner
        closeButton->setStyleSheet("font-size: 16px; background-color: red; color: white;"); // Optional styling

        // Connect the close button click to close the image window
        connect(closeButton, &QPushButton::clicked, [imageLabel, closeButton]() {
            imageLabel->close(); // Close the image window
            closeButton->close(); // Close the close button
        });

        imageLabel->show(); // Show the label containing the image
        closeButton->show(); // Show the close button
    }

void guiWindow::on_lgTipsBtn_clicked()
{
    serialPort.write("LED_SETUP_CMD"); // Command for LED setup (replace with your actual command)
        QLabel *imageLabel = new QLabel(this); // Create QLabel to hold the image
        QPixmap pixmap(":/images/setup/LG-Tips.png"); // Load the image for LED setup

        // Scale the image to the size of the window or screen
        pixmap = pixmap.scaled(this->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);

        imageLabel->setPixmap(pixmap); // Set the scaled image to the label
        imageLabel->setAlignment(Qt::AlignCenter); // Center the image in the label
        imageLabel->setGeometry(0, 0, this->width(), this->height()); // Set label to cover the entire window

        // Create the close button
        QPushButton *closeButton = new QPushButton("Close", this);
        closeButton->setGeometry(this->width() - 100, this->height() - 50, 80, 30); // Position the button in the bottom right corner
        closeButton->setStyleSheet("font-size: 16px; background-color: red; color: white;"); // Optional styling

        // Connect the close button click to close the image window
        connect(closeButton, &QPushButton::clicked, [imageLabel, closeButton]() {
            imageLabel->close(); // Close the image window
            closeButton->close(); // Close the close button
        });

        imageLabel->show(); // Show the label containing the image
        closeButton->show(); // Show the close button
    }
void guiWindow::on_lgSetupBtn_clicked()
{
    serialPort.write("LG_SETUP_CMD"); // Command for LG setup (replace with your actual command)
        QLabel *imageLabel = new QLabel(this); // Create QLabel to hold the image
        QPixmap pixmap(":/images/setup/LG-Setup.png"); // Load the image for LG setup

        // Scale the image to the size of the window or screen
        pixmap = pixmap.scaled(this->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);

        imageLabel->setPixmap(pixmap); // Set the scaled image to the label
        imageLabel->setAlignment(Qt::AlignCenter); // Center the image in the label
        imageLabel->setGeometry(0, 0, this->width(), this->height()); // Set label to cover the entire window

        // Create the close button
        QPushButton *closeButton = new QPushButton("Close", this);
        closeButton->setGeometry(this->width() - 100, this->height() - 50, 80, 30); // Position the button in the bottom right corner
        closeButton->setStyleSheet("font-size: 16px; background-color: red; color: white;"); // Optional styling

        // Connect the close button click to close the image window
        connect(closeButton, &QPushButton::clicked, [imageLabel, closeButton]() {
            imageLabel->close(); // Close the image window
            closeButton->close(); // Close the close button
        });

        imageLabel->show(); // Show the label containing the image
        closeButton->show(); // Show the close button
    }


void guiWindow::PopupWindow(QString errorTitle, QString errorMessage, QString windowTitle, int errorType)
{
    QMessageBox messageBox;
    messageBox.setText(errorTitle);
    messageBox.setInformativeText(errorMessage);
    messageBox.setWindowTitle(windowTitle);
    switch(errorType) {
    case 0:
        // lol nothing here
        break;
    case 1:
        messageBox.setIcon(QMessageBox::Question);
        break;
    case 2:
        messageBox.setIcon(QMessageBox::Information);
        break;
    case 3:
        messageBox.setIcon(QMessageBox::Warning);
        break;
    case 4:
        messageBox.setIcon(QMessageBox::Critical);
        break;
    }
    messageBox.exec();
    // TODO: maybe we should be using Serial Port errors instead of assuming,
    // but for now just clear it here for cleanliness.
    serialPort.clearError();
}

// TODO: Copy loaded values to use for comparison to determine state of save button.
void guiWindow::SerialLoad()
{
    serialActive = true;
    serialPort.write("Xlb");
    if(serialPort.waitForBytesWritten(2000)) {
        if(serialPort.waitForReadyRead(2000)) {
            // booleans
            QString buffer;
            for(uint8_t i = 1; i < sizeof(boolSettings); i++) {
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                boolSettings[i] = buffer.toInt();
                boolSettings_orig[i] = boolSettings[i];
            }
            // pins
            serialPort.write("Xlp");
            serialPort.waitForReadyRead(1000);
            buffer = serialPort.readLine();
            buffer = buffer.trimmed();
            boolSettings[customPins] = buffer.toInt(); // remember to change this BACK, teehee
            boolSettings_orig[customPins] = boolSettings[customPins];
            if(boolSettings[customPins]) {
                for(uint8_t i = 0; i < INPUTS_COUNT; i++) {
                    buffer = serialPort.readLine();
                    inputsMap_orig[i] = buffer.toInt();
                    // For some reason, QTSerial drops output shortly after this.
                    // So we send a ping to refill the buffer.
                    if(i == 14) {
                        serialPort.write(".");
                        serialPort.waitForReadyRead(1000);
                    }
                }
                inputsMap = inputsMap_orig;
            } else {
                // TODO: fix this in the firmware.
                for(uint8_t i = 0; i < INPUTS_COUNT; i++) {
                    buffer = serialPort.readLine(); // nomfing
                    inputsMap[i] = -1;
                    inputsMap_orig[i] = -1;
                    if(i == 14) {
                        serialPort.write(".");
                        serialPort.waitForReadyRead(1000);
                    }
                }
            }
            buffer = serialPort.readLine();
            buffer = buffer.trimmed();
            if(buffer != "-127") {
                qDebug() << "Padding bit not detected!";
                return;
            }
            // settings
            serialPort.write("Xls");
            serialPort.waitForBytesWritten(2000);
            serialPort.waitForReadyRead(2000);
            for(uint8_t i = 0; i < 8; i++) {
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                settingsTable[i] = buffer.toInt();
                settingsTable_orig[i] = settingsTable[i];
            }
            // profiles
            for(uint8_t i = 0; i < 4; i++) {
                QString genString = QString("XlP%1").arg(i);
                serialPort.write(genString.toLocal8Bit());
                serialPort.waitForBytesWritten(2000);
                serialPort.waitForReadyRead(2000);
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                xScale[i]->setText(buffer);
                profilesTable[i].xScale = buffer.toInt();
                profilesTable_orig[i].xScale = profilesTable[i].xScale;
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                yScale[i]->setText(buffer);
                profilesTable[i].yScale = buffer.toInt();
                profilesTable_orig[i].yScale = profilesTable[i].yScale;
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                xCenter[i]->setText(buffer);
                profilesTable[i].xCenter = buffer.toInt();
                profilesTable_orig[i].xCenter = profilesTable[i].xCenter;
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                yCenter[i]->setText(buffer);
                profilesTable[i].yCenter = buffer.toInt();
                profilesTable_orig[i].yCenter = profilesTable[i].yCenter;
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                profilesTable[i].irSensitivity = buffer.toInt();
                profilesTable_orig[i].irSensitivity = profilesTable[i].irSensitivity;
                irSens[i]->setCurrentIndex(profilesTable[i].irSensitivity);
                irSensOldIndex[i] = profilesTable[i].irSensitivity;
                buffer = serialPort.readLine();
                buffer = buffer.trimmed();
                profilesTable[i].runMode = buffer.toInt();
                profilesTable_orig[i].runMode = profilesTable[i].runMode;
                runMode[i]->setCurrentIndex(profilesTable[i].runMode);
                runModeOldIndex[i] = profilesTable[i].runMode;
            }
            serialActive = false;
        } else {
            PopupWindow("Data hasn't arrived!", "Device was detected, but settings request wasn't received in time!\nThis can happen if the app was closed in the middle of an operation.\n\nTry selecting the device again.", "Oops!", 4);
            //qDebug() << "Didn't receive any data in time!";
        }
    } else {
        qDebug() << "Couldn't send any data in time! Does the port even exist???";
    }
}

// Bool returns success (false if failed)
// TODO: copy TinyUSB values to a backup for comparison to determine availability of save btn functionality
bool guiWindow::SerialInit(int portNum)
{
    serialPort.setPort(serialFoundList[portNum]);
    serialPort.setBaudRate(QSerialPort::Baud9600);
    if(serialPort.open(QIODevice::ReadWrite)) {
        qDebug() << "Opened port successfully!";
        serialActive = true;
        // windows needs DTR enabled to actually read responses.
        serialPort.setDataTerminalReady(true);
        serialPort.write("XP");
        if(serialPort.waitForBytesWritten(2000)) {
            if(serialPort.waitForReadyRead(2000)) {
                QString buffer = serialPort.readLine();
                // if(buffer.contains("P.I.G.S")) {
                    qDebug() << "P.I.G.S detected!";
                    buffer = serialPort.readLine();
                    buffer = buffer.trimmed();
                    board.versionNumber = buffer.toFloat();
                    qDebug() << "Version number:" << board.versionNumber;
                    buffer = serialPort.readLine();
                    board.versionCodename = buffer.trimmed();
                    qDebug() << "Version codename:" << board.versionCodename;
                    buffer = serialPort.readLine();
                    buffer = buffer.trimmed();
                    if(buffer == "rpipico") {
                        board.type = rpipico;
                    } else if(buffer == "adafruitItsyRP2040") {
                        board.type = adafruitItsyRP2040;
                    } else if(buffer == "adafruitKB2040") {
                        board.type = adafruitKB2040;
                    } else if(buffer == "arduinoNanoRP2040") {
                        board.type = arduinoNanoRP2040;
                    } else {
                        board.type = generic;
                    }
                    //qDebug() << "Selected profile number:" << buffer;
                    buffer = serialPort.readLine();
                    buffer = buffer.trimmed();
                    board.selectedProfile = buffer.toInt();
                    board.previousProfile = board.selectedProfile;
                    selectedProfile[board.selectedProfile]->setChecked(true);
                    //qDebug() << "Board type:" << buffer;
                    serialPort.write("Xln");
                    serialPort.waitForReadyRead(1000);
                    buffer = serialPort.readLine();
                    if(buffer.trimmed() == "SERIALREADERR01") {
                        tinyUSBtable.tinyUSBname = "";
                    } else {
                        tinyUSBtable.tinyUSBname = buffer.trimmed();
                    }
                    tinyUSBtable_orig.tinyUSBname = tinyUSBtable.tinyUSBname;
                    serialPort.write("Xli");
                    serialPort.waitForReadyRead(1000);
                    buffer = serialPort.readLine();
                    tinyUSBtable.tinyUSBid = buffer.trimmed();
                    tinyUSBtable_orig.tinyUSBid = tinyUSBtable.tinyUSBid;
                    SerialLoad();
                    return true;
                // } else {
                    // qDebug() << "Port did not respond with expected response!";
                    // return false;
                // }
            } else {
                // PopupWindow("Data hasn't arrived!", "Device was detected, but initial settings request wasn't received in time!\nThis can happen if the app was unexpectedly closed and the gun is in a stale docked state.\n\nTry selecting the device again.", "Oops!", 3);
                qDebug() << "Didn't receive any data in time!";
                return true;
                // return false;
            }
        } else {
            qDebug() << "Couldn't send any data in time! Does the port even exist???";
            return false;
        }
    } else {
        qDebug()<<"serial port error: " << serialPort.error();
        PopupWindow("Couldn't open port!", "This usually indicates that the port is being used by something else, e.g. Arduino IDE's serial monitor, or another command line app (stty, screen).\n\nPlease close the offending application and try selecting this port again.", "Oops!", 3);
        return false;
    }
}


void guiWindow::BoxesUpdate()
{
    for(uint8_t i = 0; i < 30; i++) {
        pinBoxes[i]->setCurrentIndex(btnUnmapped);
        pinBoxesOldIndex[i] = btnUnmapped;
    }
    if(boolSettings[customPins]) {
        currentPins.clear();
        for(uint8_t i = 0; i < 30; i++) {
            pinBoxes[i]->setEnabled(true);
            currentPins[i] = btnUnmapped;
        }
        inputsMap = inputsMap_orig;
        for(uint8_t i = 0; i < INPUTS_COUNT; i++) {
            if(inputsMap.value(i) >= 0) {
                currentPins[inputsMap.value(i)] = i+1;
                pinBoxes[inputsMap.value(i)]->setCurrentIndex(currentPins[inputsMap.value(i)]);
                pinBoxesOldIndex[inputsMap.value(i)] = currentPins[inputsMap.value(i)];
            }
        }
        return;
    } else {
        switch(board.type) {
        case rpipico:
        {
            for(uint8_t i = 0; i < 30; i++) {
                currentPins[i] = rpipicoLayout[i].pinAssignment;
                pinBoxes[i]->setCurrentIndex(currentPins[i]);
                pinBoxesOldIndex[i] = currentPins[i];
                pinBoxes[i]->setEnabled(false);
            }
            return;
            break;
        }
        case adafruitItsyRP2040:
        {
            for(uint8_t i = 0; i < 30; i++) {
                currentPins[i] = adafruitItsyRP2040Layout[i].pinAssignment;
                pinBoxes[i]->setCurrentIndex(currentPins[i]);
                pinBoxesOldIndex[i] = currentPins[i];
                pinBoxes[i]->setEnabled(false);
            }
            return;
            break;
        }
        case adafruitKB2040:
        {
            for(uint8_t i = 0; i < 30; i++) {
                currentPins[i] = adafruitKB2040Layout[i].pinAssignment;
                pinBoxes[i]->setCurrentIndex(currentPins[i]);
                pinBoxesOldIndex[i] = currentPins[i];
                pinBoxes[i]->setEnabled(false);
            }
            return;
            break;
        }
        case arduinoNanoRP2040:
        {
            for(uint8_t i = 0; i < 30; i++) {
                currentPins[i] = arduinoNanoRP2040Layout[i].pinAssignment;
                pinBoxes[i]->setCurrentIndex(currentPins[i]);
                pinBoxesOldIndex[i] = currentPins[i];
                pinBoxes[i]->setEnabled(false);
            }
            return;
            break;
        }
        }
    }
}


void guiWindow::DiffUpdate()
{
    settingsDiff = 0;
    if(boolSettings_orig[customPins] != boolSettings[customPins]) {
        //settingsDiff++;
    }
    if(boolSettings[customPins]) {
        if(inputsMap_orig != inputsMap) {
            settingsDiff++;
        }
    }
    for(uint8_t i = 1; i < sizeof(boolSettings); i++) {
        if(boolSettings_orig[i] != boolSettings[i]) {
            settingsDiff++;
        }
    }
    for(uint8_t i = 0; i < sizeof(settingsTable) / 2; i++) {
        if(settingsTable_orig[i] != settingsTable[i]) {
            settingsDiff++;
        }
    }
    if(tinyUSBtable_orig.tinyUSBid != tinyUSBtable.tinyUSBid) {
        settingsDiff++;
    }
    if(tinyUSBtable_orig.tinyUSBname != tinyUSBtable.tinyUSBname) {
        settingsDiff++;
    }
    if(board.selectedProfile != board.previousProfile) {
        settingsDiff++;
    }
    for(uint8_t i = 0; i < 4; i++) {
        if(profilesTable_orig[i].xScale != profilesTable[i].xScale) {
            settingsDiff++;
        }
        if(profilesTable_orig[i].yScale != profilesTable[i].yScale) {
            settingsDiff++;
        }
        if(profilesTable_orig[i].xCenter != profilesTable[i].xCenter) {
            settingsDiff++;
        }
        if(profilesTable_orig[i].yCenter != profilesTable[i].yCenter) {
            settingsDiff++;
        }
        if(profilesTable_orig[i].irSensitivity != profilesTable[i].irSensitivity) {
            settingsDiff++;
        }
        if(profilesTable_orig[i].runMode != profilesTable[i].runMode) {
            settingsDiff++;
        }
    }
    if(settingsDiff) {
        ui->confirmButton->setText("Click To Save & Send Settings To LightGun");
        ui->confirmButton->setEnabled(true);
    } else {
        ui->confirmButton->setText("Click To Save Settings [Nothing To Save Currently]");
        ui->confirmButton->setEnabled(false);
    }
}


void guiWindow::SyncSettings()
{
    for(uint8_t i = 0; i < sizeof(boolSettings); i++) {
        boolSettings_orig[i] = boolSettings[i];
    }
    if(boolSettings_orig[customPins]) {
        inputsMap_orig = inputsMap;
    } else {
        for(uint8_t i = 0; i < INPUTS_COUNT; i++)
            inputsMap_orig[i] = -1;
    }
    for(uint8_t i = 0; i < sizeof(settingsTable) / 2; i++) {
        settingsTable_orig[i] = settingsTable[i];
    }
    tinyUSBtable_orig.tinyUSBid = tinyUSBtable.tinyUSBid;
    tinyUSBtable_orig.tinyUSBname = tinyUSBtable.tinyUSBname;
    board.previousProfile = board.selectedProfile;
    for(uint8_t i = 0; i < 4; i++) {
        profilesTable_orig[i].irSensitivity = profilesTable[i].irSensitivity;
        profilesTable_orig[i].runMode = profilesTable[i].runMode;
    }
}


QString PrettifyName()
{
    QString name;
    // if(!tinyUSBtable.tinyUSBname.isEmpty()) {
    //     name = tinyUSBtable.tinyUSBname;
    // } else {
    //     name = "Unnamed Device";
    // }
    switch(board.type) {
    case nothing:
        name = "";
        break;
    case rpipico:
        name = name + "Raspberry Pi Pico";
        break;
    case adafruitItsyRP2040:
        name = name + "Adafruit ItsyBitsy RP2040";
        break;
    case adafruitKB2040:
        name = name + "Adafruit KB2040";
        break;
    case arduinoNanoRP2040:
        name = name + "Arduino Nano RP2040 Connect";
        break;
    case generic:
        name = name + "LG2040";
        break;
    }
    return name;
}


void guiWindow::on_confirmButton_clicked()
{
    QMessageBox messageBox;
    messageBox.setText("Are these settings okay?");
    messageBox.setInformativeText("These settings will be committed to your lightgun. Is that okay?");
    messageBox.setWindowTitle("Commit Confirmation");
    messageBox.setIcon(QMessageBox::Information);
    messageBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    messageBox.setDefaultButton(QMessageBox::Yes);
    int value = messageBox.exec();
    if(value == QMessageBox::Yes) {
        if(serialPort.isOpen()) {
            serialActive = true;
            // send a signal so the gun pauses its test outputs for the save op.
            serialPort.write("Xm");
            serialPort.waitForBytesWritten(1000);

            QProgressBar *statusProgressBar = new QProgressBar();
            ui->statusBar->addPermanentWidget(statusProgressBar);
            // ui->tabWidget->setEnabled(false);
            ui->comPortSelector->setEnabled(false);
            ui->confirmButton->setEnabled(false);

            QStringList serialQueue;
            for(uint8_t i = 1; i < sizeof(boolSettings); i++) {
                QString genString = QString("Xm.0.%1.%2").arg(i-1).arg(boolSettings[i]);
                serialQueue.append(genString);
            }

            if(boolSettings[customPins]) {
                serialQueue.append("Xm.1.0.1");
                for(uint8_t i = 0; i < INPUTS_COUNT; i++) {
                    QString genString = QString("Xm.1.%1.%2").arg(i+1).arg(inputsMap.value(i));
                    serialQueue.append(genString);
                }
            } else {
                serialQueue.append("Xm.1.0.0");
            }

            for(uint8_t i = 0; i < sizeof(settingsTable) / 2; i++) {
                QString genString = QString("Xm.2.%1.%2").arg(i).arg(settingsTable[i]);
                serialQueue.append(genString);
            }

            serialQueue.append(QString("Xm.3.0.%1").arg(tinyUSBtable.tinyUSBid));
            if(!tinyUSBtable.tinyUSBname.isEmpty()) {
                serialQueue.append(QString("Xm.3.1.%1").arg(tinyUSBtable.tinyUSBname));
            }
            for(uint8_t i = 0; i < 4; i++) {
                serialQueue.append(QString("Xm.P.i.%1.%2").arg(i).arg(profilesTable[i].irSensitivity));
                serialQueue.append(QString("Xm.P.r.%1.%2").arg(i).arg(profilesTable[i].runMode));
            }
            serialQueue.append("XS");

            statusProgressBar->setRange(0, serialQueue.length()-1);
            bool success = true;

            // throw out whatever's in the buffer if there's anything there.
            while(!serialPort.atEnd()) {
                serialPort.readLine();
            }

            for(uint8_t i = 0; i < serialQueue.length(); i++) {
                serialPort.write(serialQueue[i].toLocal8Bit());
                serialPort.waitForBytesWritten(2000);
                if(serialPort.waitForReadyRead(2000)) {
                    QString buffer = serialPort.readLine();
                    if(buffer.contains("OK:") || buffer.contains("NOENT:")) {
                        statusProgressBar->setValue(statusProgressBar->value() + 1);
                    } else if(i == serialQueue.length() - 1 && buffer.contains("Saving preferences...")) {
                        buffer = serialPort.readLine();
                        if(buffer.contains("Settings saved to")) {
                            success = true;
                            // because there's probably some leftover bytes that might congest things:
                            while(!serialPort.atEnd()) {
                                serialPort.readLine();
                            }
                        } else {
                            qDebug() << "Sent save command, but didn't save successfully! What the fuck happened???";
                        }
                    } else {
                        success = false;
                        break;
                    }
                }
            }
            ui->statusBar->removeWidget(statusProgressBar);
            delete statusProgressBar;
            // ui->tabWidget->setEnabled(true);
            ui->comPortSelector->setEnabled(true);
            if(!success) {
                qDebug() << "Setting save failed, it failed!";
            } else {
                statusBar()->showMessage("Sent settings successfully!", 5000);
                SyncSettings();
                DiffUpdate();
                ui->boardLabel->setText(PrettifyName());
            }
            serialActive = false;
            serialQueue.clear();
            if(!serialPort.atEnd()) {
                serialPort.readAll();
            }
        } else {
            qDebug() << "Wait, this port wasn't open to begin with!!! WTF SEONG!?!?";
        }
    } else {
        statusBar()->showMessage("Save operation canceled.", 3000);
    }
}


void guiWindow::on_comPortSelector_currentIndexChanged(int index)
{
    // Indiscriminately clears the board layout views.
    // yes, every time. goddammit QT.
    // fuck it, it works until QT provides a better mechanism to remove widgets without deleting them.
    if(pinBoxes[0]->count() > 0) {
        for(uint8_t i = 0; i < 30; i++) {
            pinBoxes[i]->clear();
            delete pinBoxes[i];
            delete padding[i];
            delete pinLabel[i];
        }
        delete centerPic;
    }

    // delete PinsCenter;
    // delete PinsLeft;
    // delete PinsRight;

    // PinsCenter = new QVBoxLayout();
    // PinsCenterSub = new QGridLayout();
    // PinsLeft = new QGridLayout();
    // PinsRight = new QGridLayout();

    // ui->PinsTopHalf->addLayout(PinsLeft);
    // ui->PinsTopHalf->addLayout(PinsCenter);
    // ui->PinsTopHalf->addLayout(PinsRight);

    for(uint8_t i = 0; i < 30; i++) {
        pinBoxes[i] = new QComboBox();
        pinBoxes[i]->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
        connect(pinBoxes[i], SIGNAL(activated(int)), this, SLOT(pinBoxes_activated(int)));
        padding[i] = new QWidget();
        padding[i]->setMinimumHeight(25);
        pinLabel[i] = new QLabel(QString("<GPIO%1>").arg(i));
        pinLabel[i]->setEnabled(false);
        pinLabel[i]->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    if(index > 0) {
        qDebug() << "COM port set to" << ui->comPortSelector->currentIndex();
        // Clear stale states if any, and unmount old board if mounted.
        if(testMode) {
            testMode = false;
            ui->testView->setEnabled(false);
            ui->buttonsTestArea->setEnabled(true);
            ui->testBtn->setText("Enable IR Test Mode");
            // ui->pinsTab->setEnabled(true);
            ui->settingsTab->setEnabled(true);
            ui->profilesTab->setEnabled(true);
            ui->feedbackTestsBox->setEnabled(true);
            ui->dangerZoneBox->setEnabled(true);
            serialActive = false;
        }
        if(serialPort.isOpen()) {
            serialActive = true;
            serialPort.write("XE");
            serialPort.waitForBytesWritten(2000);
            serialPort.waitForReadyRead(2000);
            serialPort.readAll();
            serialPort.close();
            serialActive = false;
        }
        if(!SerialInit(index - 1)) {
            ui->comPortSelector->setCurrentIndex(0);
        } else {
            // switch(board.type) {
            // case rpipico:
            // {
            //     // update box types
            //     for(uint8_t i = 0; i < 30; i++) {
            //         pinBoxes[i]->addItems(valuesNameList);
            //         if(rpipicoLayout[i].pinType == pinDigital) {
            //             pinBoxes[i]->removeItem(25);
            //             pinBoxes[i]->removeItem(24);
            //             // replace "Temp Sensor" with a separator
            //             // then remove the presumably bumped up temp sensor index.
            //             pinBoxes[i]->insertSeparator(16);
            //             pinBoxes[i]->removeItem(17);
            //         }
            //     }

            //     BoxesUpdate();

            //     centerPic = new QSvgWidget(":/boardPics/pico.svg");
            //     QSvgRenderer *picRenderer = centerPic->renderer();
            //     picRenderer->setAspectRatioMode(Qt::KeepAspectRatio);
            //     ui->boardLabel->setText(PrettifyName());

            //     // left side (has 1 row of top padding)
            //     // PinsLeft->addWidget(padding[0], 0, 1);
            //     // PinsLeft->addWidget(pinBoxes[0], 1, 0),
            //     //     PinsLeft->addWidget(pinLabel[0], 1, 1);
            //     // PinsLeft->addWidget(pinBoxes[1], 2, 0),
            //     //     PinsLeft->addWidget(pinLabel[1], 2, 1);
            //     // PinsLeft->addWidget(padding[1], 3, 1);
            //     // PinsLeft->addWidget(pinBoxes[2], 4, 0),
            //     //     PinsLeft->addWidget(pinLabel[2], 4, 1);
            //     // PinsLeft->addWidget(pinBoxes[3], 5, 0),
            //     //     PinsLeft->addWidget(pinLabel[3], 5, 1);
            //     // PinsLeft->addWidget(pinBoxes[4], 6, 0),
            //     //     PinsLeft->addWidget(pinLabel[4], 6, 1);
            //     // PinsLeft->addWidget(pinBoxes[5], 7, 0),
            //     //     PinsLeft->addWidget(pinLabel[5], 7, 1);
            //     // PinsLeft->addWidget(padding[2], 8, 1);
            //     // PinsLeft->addWidget(pinBoxes[6], 9, 0),
            //     //     PinsLeft->addWidget(pinLabel[6], 9, 1);
            //     // PinsLeft->addWidget(pinBoxes[7], 10, 0),
            //     //     PinsLeft->addWidget(pinLabel[7], 10, 1);
            //     // PinsLeft->addWidget(pinBoxes[8], 11, 0),
            //     //     PinsLeft->addWidget(pinLabel[8], 11, 1);
            //     // PinsLeft->addWidget(pinBoxes[9], 12, 0),
            //     //     PinsLeft->addWidget(pinLabel[9], 12, 1);
            //     // PinsLeft->addWidget(padding[3], 13, 1);
            //     // PinsLeft->addWidget(pinBoxes[10], 14, 0),
            //     //     PinsLeft->addWidget(pinLabel[10], 14, 1);
            //     // PinsLeft->addWidget(pinBoxes[11], 15, 0, 1, 1),
            //     //     PinsLeft->addWidget(pinLabel[11], 15, 1);
            //     // PinsLeft->addWidget(pinBoxes[12], 16, 0),
            //     //     PinsLeft->addWidget(pinLabel[12], 16, 1);
            //     // PinsLeft->addWidget(pinBoxes[13], 17, 0),
            //     //     PinsLeft->addWidget(pinLabel[13], 17, 1);
            //     // PinsLeft->addWidget(padding[4], 18, 1);
            //     // PinsLeft->addWidget(pinBoxes[14], 19, 0),
            //     //     PinsLeft->addWidget(pinLabel[14], 19, 1);
            //     // PinsLeft->addWidget(pinBoxes[15], 20, 0),
            //     //     PinsLeft->addWidget(pinLabel[15], 20, 1);

            //     // // right side (has 1 row of top padding)
            //     // PinsRight->addWidget(padding[5], 0, 0); // top
            //     // PinsRight->addWidget(padding[6], 1, 1);
            //     // PinsRight->addWidget(padding[7], 2, 1);
            //     // PinsRight->addWidget(padding[8], 3, 1); // gnd
            //     // PinsRight->addWidget(padding[9], 4, 1);
            //     // PinsRight->addWidget(padding[10], 5, 1);
            //     // PinsRight->addWidget(padding[11], 6, 1);
            //     // PinsRight->addWidget(pinBoxes[28], 7, 1),
            //     //     PinsRight->addWidget(pinLabel[28], 7, 0);
            //     // PinsRight->addWidget(padding[12], 8, 0); // gnd
            //     // PinsRight->addWidget(pinBoxes[27], 9, 1),
            //     //     PinsRight->addWidget(pinLabel[27], 9, 0);
            //     // PinsRight->addWidget(pinBoxes[26], 10, 1),
            //     //     PinsRight->addWidget(pinLabel[26], 10, 0);
            //     // PinsRight->addWidget(padding[13], 11, 1);
            //     // PinsRight->addWidget(pinBoxes[22], 12, 1),
            //     //     PinsRight->addWidget(pinLabel[22], 12, 0);
            //     // PinsRight->addWidget(padding[14], 13, 0); // gnd
            //     // PinsRight->addWidget(padding[15], 14, 1);     // data
            //     // PinsRight->addWidget(padding[16], 15, 1);     // clock
            //     // PinsRight->addWidget(pinBoxes[19], 16, 1),
            //     //     PinsRight->addWidget(pinLabel[19], 16, 0);
            //     // PinsRight->addWidget(pinBoxes[18], 17, 1),
            //     //     PinsRight->addWidget(pinLabel[18], 17, 0);
            //     // PinsRight->addWidget(padding[17], 18, 0); // gnd
            //     // PinsRight->addWidget(pinBoxes[17], 19, 1),
            //     //     PinsRight->addWidget(pinLabel[17], 19, 0);
            //     // PinsRight->addWidget(pinBoxes[16], 20, 1),
            //     //     PinsRight->addWidget(pinLabel[16], 20, 0);

            //     // // center
            //     // PinsCenter->addWidget(centerPic);
            //     // centerPic->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
            //     break;
            // }
            // case adafruitItsyRP2040:
            // {
            //     // update box types
            //     for(uint8_t i = 0; i < 30; i++) {
            //         pinBoxes[i]->addItems(valuesNameList);
            //         if(adafruitItsyRP2040Layout[i].pinType == pinDigital) {
            //             pinBoxes[i]->removeItem(25);
            //             pinBoxes[i]->removeItem(24);
            //             // replace "Temp Sensor" with a separator
            //             // then remove the presumably bumped up temp sensor index.
            //             pinBoxes[i]->insertSeparator(16);
            //             pinBoxes[i]->removeItem(17);
            //         }
            //     }

            //     BoxesUpdate();

            //     centerPic = new QSvgWidget(":/boardPics/adafruitItsy2040.svg");
            //     QSvgRenderer *picRenderer = centerPic->renderer();
            //     picRenderer->setAspectRatioMode(Qt::KeepAspectRatio);
            //     ui->boardLabel->setText(PrettifyName());

            //     // left side
            //     PinsLeft->addWidget(padding[0], 0, 1);
            //     PinsLeft->addWidget(padding[1], 1, 1);
            //     PinsLeft->addWidget(padding[3], 2, 1);
            //     PinsLeft->addWidget(padding[4], 3, 1);
            //     PinsLeft->addWidget(pinBoxes[26], 4, 0),
            //         PinsLeft->addWidget(pinLabel[26], 4, 1);
            //     PinsLeft->addWidget(pinBoxes[27], 5, 0),
            //         PinsLeft->addWidget(pinLabel[27], 5, 1);
            //     PinsLeft->addWidget(pinBoxes[28], 6, 0),
            //         PinsLeft->addWidget(pinLabel[28], 6, 1);
            //     PinsLeft->addWidget(pinBoxes[29], 7, 0),
            //         PinsLeft->addWidget(pinLabel[29], 7, 1);
            //     PinsLeft->addWidget(pinBoxes[24], 8, 0),
            //         PinsLeft->addWidget(pinLabel[24], 8, 1);
            //     PinsLeft->addWidget(pinBoxes[25], 9, 0),
            //         PinsLeft->addWidget(pinLabel[25], 9, 1);
            //     PinsLeft->addWidget(pinBoxes[18], 10, 0),
            //         PinsLeft->addWidget(pinLabel[18], 10, 1);
            //     PinsLeft->addWidget(pinBoxes[19], 11, 0),
            //         PinsLeft->addWidget(pinLabel[19], 11, 1);
            //     PinsLeft->addWidget(pinBoxes[20], 12, 0),
            //         PinsLeft->addWidget(pinLabel[20], 12, 1);
            //     PinsLeft->addWidget(pinBoxes[12], 13, 0),
            //         PinsLeft->addWidget(pinLabel[12], 13, 1);
            //     PinsLeft->addWidget(padding[5], 14, 0),
            //         PinsLeft->addWidget(padding[6], 14, 1);
            //     PinsLeft->addWidget(padding[7], 15, 1);

            //     // right side
            //     PinsRight->addWidget(padding[8], 0, 1);
            //     PinsRight->addWidget(padding[9], 1, 0),
            //         PinsRight->addWidget(padding[10], 2, 1);
            //     PinsRight->addWidget(pinBoxes[11], 3, 1),
            //         PinsRight->addWidget(pinLabel[11], 3, 0);
            //     PinsRight->addWidget(pinBoxes[10], 4, 1),
            //         PinsRight->addWidget(pinLabel[10], 4, 0);
            //     PinsRight->addWidget(pinBoxes[9], 5, 1),
            //         PinsRight->addWidget(pinLabel[9], 5, 0);
            //     PinsRight->addWidget(pinBoxes[8], 6, 1),
            //         PinsRight->addWidget(pinLabel[8], 6, 0);
            //     PinsRight->addWidget(pinBoxes[7], 7, 1),
            //         PinsRight->addWidget(pinLabel[7], 7, 0);
            //     PinsRight->addWidget(pinBoxes[6], 8, 1),
            //         PinsRight->addWidget(pinLabel[6], 8, 0);
            //     PinsRight->addWidget(padding[11], 9, 1);      // 5!
            //     PinsRight->addWidget(padding[12], 10, 1);     // data
            //     PinsRight->addWidget(padding[13], 11, 1);     // clock
            //     PinsRight->addWidget(pinBoxes[0], 12, 1),
            //         PinsRight->addWidget(pinLabel[0], 12, 0);
            //     PinsRight->addWidget(pinBoxes[1], 13, 1),
            //         PinsRight->addWidget(pinLabel[1], 13, 0);
            //     PinsRight->addWidget(padding[14], 14, 1);
            //     PinsRight->addWidget(padding[15], 15, 1);

            //     // center
            //     PinsCenter->addWidget(centerPic);
            //     PinsCenter->addLayout(PinsCenterSub);
            //     centerPic->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
            //     PinsCenterSub->addWidget(pinBoxes[4], 1, 3),
            //         PinsCenterSub->addWidget(pinLabel[4], 0, 3);
            //     PinsCenterSub->addWidget(pinBoxes[5], 1, 2),
            //         PinsCenterSub->addWidget(pinLabel[5], 0, 2);
            //     break;
            // }
            // case adafruitKB2040:
            // {
            //     // update box types
            //     for(uint8_t i = 0; i < 30; i++) {
            //         pinBoxes[i]->addItems(valuesNameList);
            //         if(adafruitKB2040Layout[i].pinType == pinDigital) {
            //             pinBoxes[i]->removeItem(25);
            //             pinBoxes[i]->removeItem(24);
            //             // replace "Temp Sensor" with a separator
            //             // then remove the presumably bumped up temp sensor index.
            //             pinBoxes[i]->insertSeparator(16);
            //             pinBoxes[i]->removeItem(17);
            //         }
            //     }

            //     BoxesUpdate();

            //     centerPic = new QSvgWidget(":/boardPics/adafruitKB2040.svg");
            //     QSvgRenderer *picRenderer = centerPic->renderer();
            //     picRenderer->setAspectRatioMode(Qt::KeepAspectRatio);
            //     ui->boardLabel->setText(PrettifyName());

            //     // left side
            //     PinsLeft->addWidget(padding[0], 0, 1);        // D+
            //     PinsLeft->addWidget(pinBoxes[0], 1, 0),
            //         PinsLeft->addWidget(pinLabel[0], 1, 1);
            //     PinsLeft->addWidget(pinBoxes[1], 2, 0),
            //         PinsLeft->addWidget(pinLabel[1], 2, 1);
            //     PinsLeft->addWidget(padding[1], 3, 1);        // gnd
            //     PinsLeft->addWidget(padding[2], 4, 1);        // gnd
            //     PinsLeft->addWidget(padding[3], 5, 1);        // data
            //     PinsLeft->addWidget(padding[4], 6, 1);        // clock
            //     PinsLeft->addWidget(pinBoxes[4], 7, 0),
            //         PinsLeft->addWidget(pinLabel[4], 7, 1);
            //     PinsLeft->addWidget(pinBoxes[5], 8, 0),
            //         PinsLeft->addWidget(pinLabel[5], 8, 1);
            //     PinsLeft->addWidget(pinBoxes[6], 9, 0),
            //         PinsLeft->addWidget(pinLabel[6], 9, 1);
            //     PinsLeft->addWidget(pinBoxes[7], 10, 0),
            //         PinsLeft->addWidget(pinLabel[7], 10, 1);
            //     PinsLeft->addWidget(pinBoxes[8], 11, 0),
            //         PinsLeft->addWidget(pinLabel[8], 11, 1);
            //     PinsLeft->addWidget(pinBoxes[9], 12, 0),
            //         PinsLeft->addWidget(pinLabel[9], 12, 1);

            //     // right side
            //     PinsRight->addWidget(padding[5], 0, 0);       // D-
            //     PinsRight->addWidget(padding[6], 1, 0);       // RAW
            //     PinsRight->addWidget(padding[7], 2, 0);       // gnd
            //     PinsRight->addWidget(padding[8], 3, 0);       // reset
            //     PinsRight->addWidget(padding[9], 4, 0);       // 3.3v
            //     PinsRight->addWidget(pinBoxes[29], 5, 1),
            //         PinsRight->addWidget(pinLabel[29], 5, 0);
            //     PinsRight->addWidget(pinBoxes[28], 6, 1),
            //         PinsRight->addWidget(pinLabel[28], 6, 0);
            //     PinsRight->addWidget(pinBoxes[27], 7, 1),
            //         PinsRight->addWidget(pinLabel[27], 7, 0);
            //     PinsRight->addWidget(pinBoxes[26], 8, 1),
            //         PinsRight->addWidget(pinLabel[26], 8, 0);
            //     PinsRight->addWidget(pinBoxes[18], 9, 1),
            //         PinsRight->addWidget(pinLabel[18], 9, 0);
            //     PinsRight->addWidget(pinBoxes[20], 10, 1),
            //         PinsRight->addWidget(pinLabel[20], 10, 0);
            //     PinsRight->addWidget(pinBoxes[19], 11, 1),
            //         PinsRight->addWidget(pinLabel[19], 11, 0);
            //     PinsRight->addWidget(pinBoxes[10], 12, 1),
            //         PinsRight->addWidget(pinLabel[10], 12, 0);

            //     // center
            //     PinsCenter->addWidget(centerPic);
            //     centerPic->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
            //     break;
            // }
            // case arduinoNanoRP2040:
            // {
            //     for(uint8_t i = 0; i < 30; i++) {
            //         pinBoxes[i]->addItems(valuesNameList);
            //         if(arduinoNanoRP2040Layout[i].pinType == pinDigital) {
            //             pinBoxes[i]->removeItem(25);
            //             pinBoxes[i]->removeItem(24);
            //             // replace "Temp Sensor" with a separator
            //             // then remove the presumably bumped up temp sensor index.
            //             pinBoxes[i]->insertSeparator(16);
            //             pinBoxes[i]->removeItem(17);
            //         }
            //     }

            //     BoxesUpdate();

            //     centerPic = new QSvgWidget(":/boardPics/arduinoNano2040.svg");
            //     QSvgRenderer *picRenderer = centerPic->renderer();
            //     picRenderer->setAspectRatioMode(Qt::KeepAspectRatio);
            //     PinsCenter->addWidget(centerPic);
            //     ui->boardLabel->setText(PrettifyName());

            //     // left side
            //     PinsLeft->addWidget(padding[0], 0, 0); // top bumpdown
            //     PinsLeft->addWidget(pinBoxes[6], 1, 0);
            //     PinsLeft->addWidget(pinLabel[6], 1, 1);
            //     PinsLeft->addWidget(padding[1], 2, 0);
            //     PinsLeft->addWidget(padding[2], 3, 0);
            //     PinsLeft->addWidget(pinBoxes[26], 4, 0);
            //     PinsLeft->addWidget(pinLabel[26], 4, 1);
            //     PinsLeft->addWidget(pinBoxes[27], 5, 0);
            //     PinsLeft->addWidget(pinLabel[27], 5, 1);
            //     PinsLeft->addWidget(pinBoxes[28], 6, 0);
            //     PinsLeft->addWidget(pinLabel[28], 6, 1);
            //     PinsLeft->addWidget(pinBoxes[29], 7, 0);
            //     PinsLeft->addWidget(pinLabel[29], 7, 1);
            //     PinsLeft->addWidget(padding[3], 8, 0); // data
            //     PinsLeft->addWidget(padding[4], 9, 0); // clock
            //     PinsLeft->addWidget(padding[5], 10, 0); // A6 - unused
            //     PinsLeft->addWidget(padding[6], 11, 0);// A7 - unused
            //     PinsLeft->addWidget(padding[7], 12, 0);// 5V
            //     PinsLeft->addWidget(padding[8], 13, 0);// REC?
            //     PinsLeft->addWidget(padding[9], 14, 0);// gnd
            //     PinsLeft->addWidget(padding[10], 15, 0);// 5v input
            //     PinsLeft->addWidget(padding[11], 16, 0);// bottom bumpup

            //     // right side
            //     PinsRight->addWidget(padding[12], 0, 0);
            //     PinsRight->addWidget(pinBoxes[4], 1, 1);
            //     PinsRight->addWidget(pinLabel[4], 1, 0);
            //     PinsRight->addWidget(pinBoxes[7], 2, 1);
            //     PinsRight->addWidget(pinLabel[7], 2, 0);
            //     PinsRight->addWidget(pinBoxes[5], 3, 1);
            //     PinsRight->addWidget(pinLabel[5], 3, 0);
            //     PinsRight->addWidget(pinBoxes[21], 4, 1);
            //     PinsRight->addWidget(pinLabel[21], 4, 0);
            //     PinsRight->addWidget(pinBoxes[20], 5, 1);
            //     PinsRight->addWidget(pinLabel[20], 5, 0);
            //     PinsRight->addWidget(pinBoxes[19], 6, 1);
            //     PinsRight->addWidget(pinLabel[19], 6, 0);
            //     PinsRight->addWidget(pinBoxes[18], 7, 1);
            //     PinsRight->addWidget(pinLabel[18], 7, 0);
            //     PinsRight->addWidget(pinBoxes[17], 8, 1);
            //     PinsRight->addWidget(pinLabel[17], 8, 0);
            //     PinsRight->addWidget(pinBoxes[16], 9, 1);
            //     PinsRight->addWidget(pinLabel[16], 9, 0);
            //     PinsRight->addWidget(pinBoxes[15], 10, 1);
            //     PinsRight->addWidget(pinLabel[15], 10, 0);
            //     PinsRight->addWidget(pinBoxes[25], 11, 1);
            //     PinsRight->addWidget(pinLabel[25], 11, 0);
            //     PinsRight->addWidget(padding[13], 12, 0);
            //     PinsRight->addWidget(padding[14], 13, 0);
            //     PinsRight->addWidget(pinBoxes[1], 14, 1);
            //     PinsRight->addWidget(pinLabel[1], 14, 0);
            //     PinsRight->addWidget(pinBoxes[0], 15, 1);
            //     PinsRight->addWidget(pinLabel[0], 15, 0);
            //     PinsRight->addWidget(padding[15], 16, 0);

            //     // center
            //     PinsCenter->addWidget(centerPic);
            //     break;
            // }
            // case generic:
            //     // update box types
            //     for(uint8_t i = 0; i < 30; i++) {
            //         pinBoxes[i]->addItems(valuesNameList);
            //         if(genericLayout[i].pinType == pinDigital) {
            //             pinBoxes[i]->removeItem(25);
            //             pinBoxes[i]->removeItem(24);
            //             // replace "Temp Sensor" with a separator
            //             // then remove the presumably bumped up temp sensor index.
            //             pinBoxes[i]->insertSeparator(16);
            //             pinBoxes[i]->removeItem(17);
            //         }
            //     }

            //     BoxesUpdate();

            //     centerPic = new QSvgWidget(":/boardPics/unknown.svg");
            //     QSvgRenderer *picRenderer = centerPic->renderer();
            //     picRenderer->setAspectRatioMode(Qt::KeepAspectRatio);
            //     ui->boardLabel->setText(PrettifyName());

            //     // left side (has 1 row of top padding)
            //     PinsLeft->addWidget(padding[0], 0, 1);
            //     PinsLeft->addWidget(pinBoxes[0], 1, 0),
            //         PinsLeft->addWidget(pinLabel[0], 1, 1);
            //     PinsLeft->addWidget(pinBoxes[1], 2, 0),
            //         PinsLeft->addWidget(pinLabel[1], 2, 1);
            //     PinsLeft->addWidget(padding[1], 3, 1);
            //     PinsLeft->addWidget(pinBoxes[2], 4, 0),
            //         PinsLeft->addWidget(pinLabel[2], 4, 1);
            //     PinsLeft->addWidget(pinBoxes[3], 5, 0),
            //         PinsLeft->addWidget(pinLabel[3], 5, 1);
            //     PinsLeft->addWidget(pinBoxes[4], 6, 0),
            //         PinsLeft->addWidget(pinLabel[4], 6, 1);
            //     PinsLeft->addWidget(pinBoxes[5], 7, 0),
            //         PinsLeft->addWidget(pinLabel[5], 7, 1);
            //     PinsLeft->addWidget(padding[2], 8, 1);
            //     PinsLeft->addWidget(pinBoxes[6], 9, 0),
            //         PinsLeft->addWidget(pinLabel[6], 9, 1);
            //     PinsLeft->addWidget(pinBoxes[7], 10, 0),
            //         PinsLeft->addWidget(pinLabel[7], 10, 1);
            //     PinsLeft->addWidget(pinBoxes[8], 11, 0),
            //         PinsLeft->addWidget(pinLabel[8], 11, 1);
            //     PinsLeft->addWidget(pinBoxes[9], 12, 0),
            //         PinsLeft->addWidget(pinLabel[9], 12, 1);
            //     PinsLeft->addWidget(padding[3], 13, 1);
            //     PinsLeft->addWidget(pinBoxes[10], 14, 0),
            //         PinsLeft->addWidget(pinLabel[10], 14, 1);
            //     PinsLeft->addWidget(pinBoxes[11], 15, 0, 1, 1),
            //         PinsLeft->addWidget(pinLabel[11], 15, 1);
            //     PinsLeft->addWidget(pinBoxes[12], 16, 0),
            //         PinsLeft->addWidget(pinLabel[12], 16, 1);
            //     PinsLeft->addWidget(pinBoxes[13], 17, 0),
            //         PinsLeft->addWidget(pinLabel[13], 17, 1);
            //     PinsLeft->addWidget(padding[4], 18, 1);
            //     PinsLeft->addWidget(pinBoxes[14], 19, 0),
            //         PinsLeft->addWidget(pinLabel[14], 19, 1);
            //     PinsLeft->addWidget(pinBoxes[15], 20, 0),
            //         PinsLeft->addWidget(pinLabel[15], 20, 1);

            //     // right side (has 1 row of top padding)
            //     PinsRight->addWidget(padding[5], 0, 0); // top
            //     PinsRight->addWidget(padding[6], 1, 1);
            //     PinsRight->addWidget(padding[7], 2, 1);
            //     PinsRight->addWidget(padding[8], 3, 1); // gnd
            //     PinsRight->addWidget(padding[9], 4, 1);
            //     PinsRight->addWidget(padding[10], 5, 1);
            //     PinsRight->addWidget(padding[11], 6, 1);
            //     PinsRight->addWidget(pinBoxes[28], 7, 1),
            //         PinsRight->addWidget(pinLabel[28], 7, 0);
            //     PinsRight->addWidget(padding[12], 8, 0); // gnd
            //     PinsRight->addWidget(pinBoxes[27], 9, 1),
            //         PinsRight->addWidget(pinLabel[27], 9, 0);
            //     PinsRight->addWidget(pinBoxes[26], 10, 1),
            //         PinsRight->addWidget(pinLabel[26], 10, 0);
            //     PinsRight->addWidget(padding[13], 11, 1);
            //     PinsRight->addWidget(pinBoxes[22], 12, 1),
            //         PinsRight->addWidget(pinLabel[22], 12, 0);
            //     PinsRight->addWidget(padding[14], 13, 0); // gnd
            //     PinsRight->addWidget(padding[15], 14, 1);     // data
            //     PinsRight->addWidget(padding[16], 15, 1);     // clock
            //     PinsRight->addWidget(pinBoxes[19], 16, 1),
            //         PinsRight->addWidget(pinLabel[19], 16, 0);
            //     PinsRight->addWidget(pinBoxes[18], 17, 1),
            //         PinsRight->addWidget(pinLabel[18], 17, 0);
            //     PinsRight->addWidget(padding[17], 18, 0); // gnd
            //     PinsRight->addWidget(pinBoxes[17], 19, 1),
            //         PinsRight->addWidget(pinLabel[17], 19, 0);
            //     PinsRight->addWidget(pinBoxes[16], 20, 1),
            //         PinsRight->addWidget(pinLabel[16], 20, 0);

            //     // center
            //     PinsCenter->addWidget(centerPic);
            //     centerPic->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
            //     break;
            // }


            // ui->tabWidget->setEnabled(true);
            // ui->customPinsEnabled->setChecked(boolSettings[customPins]);
//            ui->nunChuckToggle->setChecked(boolSettings[nunChuck]);
            ui->rumbleToggle->setChecked(boolSettings[rumble]);
            ui->solenoidToggle->setChecked(boolSettings[solenoid]);
            ui->autofireToggle->setChecked(boolSettings[autofire]);
            ui->holdToPauseToggle->setChecked(boolSettings[holdToPause]);
            ui->rumbleIntensityBox->setValue(settingsTable[rumbleStrength]);
            ui->rumbleLengthBox->setValue(settingsTable[rumbleInterval]);
            ui->holdToPauseLengthBox->setValue(settingsTable[holdToPauseLength]);
            ui->solenoidNormalIntervalBox->setValue(settingsTable[solenoidNormalInterval]);
            ui->solenoidFastIntervalBox->setValue(settingsTable[solenoidFastInterval]);
            ui->solenoidHoldLengthBox->setValue(settingsTable[solenoidHoldLength]);
            ui->autofireWaitFactorBox->setValue(settingsTable[autofireWaitFactor]);
        }
    } else {
        ui->boardLabel->clear();

        if(serialPort.isOpen()) {
            serialActive = true;
            serialPort.write("XE");
            serialPort.waitForBytesWritten(2000);
            serialPort.waitForReadyRead(2000);
            serialPort.readAll();
            serialPort.close();
            if(testMode) {
                testMode = false;
                ui->testView->setEnabled(false);
                ui->buttonsTestArea->setEnabled(true);
                ui->testBtn->setText("Enable IR Test Mode");
                // ui->pinsTab->setEnabled(true);
                ui->settingsTab->setEnabled(true);
                ui->profilesTab->setEnabled(true);
                ui->feedbackTestsBox->setEnabled(true);
                ui->dangerZoneBox->setEnabled(true);
                serialActive = false;
            }
            serialActive = false;
        }
        qDebug() << "COM port disabled!";
        // ui->tabWidget->setEnabled(false);
    }
}

void guiWindow::pinBoxes_activated(int index)
{
    // Demultiplexing to figure out which "pin" this combobox that's calling correlates to.
    uint8_t pin;
    QObject* obj = sender();
    for(uint8_t i = 0;;i++) {
        if(obj == pinBoxes[i]) {
            pin = i;
            break;
        }
    }

    if(!index) {
        inputsMap[currentPins.value(pin) - 1] = -1;
        currentPins[pin] = btnUnmapped;
    } else if(pinBoxesOldIndex[pin] != index) {
        int8_t btnRequest = index - 1;

        // Scorched Earth approach, clear anything that matches to unmapped.
        inputsMap[btnRequest] = -1;
        // only reset if current pin was already mapped.
        if(currentPins.value(pin) > 0) {
            inputsMap[currentPins.value(pin) - 1] = -1;
        }
        QList<uint8_t> foundList = currentPins.keys(index);
        for(uint8_t i = 0; i < foundList.length(); i++) {
            currentPins[foundList[i]] = btnUnmapped;
            pinBoxes[foundList[i]]->setCurrentIndex(btnUnmapped);
            pinBoxesOldIndex[foundList[i]] = btnUnmapped;
        }
        // Then map the thing.
        currentPins[pin] = index;
        inputsMap[btnRequest] = pin;
    }
    // because "->currentIndex" is already updated, we just update it at the end of activations
    // to check that we aren't re-selecting the index for that box.
    pinBoxesOldIndex[pin] = index;
    DiffUpdate();
}

void guiWindow::irBoxes_activated(int index)
{
    // Demultiplexing to figure out which "pin" this combobox that's calling correlates to.
    uint8_t slot;
    QObject* obj = sender();
    for(uint8_t i = 0;;i++) {
        if(obj == irSens[i]) {
            slot = i;
            break;
        }
    }

    if(index != irSensOldIndex[slot]) {
        profilesTable[slot].irSensitivity = index;
    }
    irSensOldIndex[slot] = index;
    DiffUpdate();
}


void guiWindow::runModeBoxes_activated(int index)
{
    // Demultiplexing to figure out which "pin" this combobox that's calling correlates to.
    uint8_t slot;
    QObject* obj = sender();
    for(uint8_t i = 0;;i++) {
        if(obj == runMode[i]) {
            slot = i;
            break;
        }
    }

    if(index != runModeOldIndex[slot]) {
        profilesTable[slot].runMode = index;
    }
    runModeOldIndex[slot] = index;
    DiffUpdate();
}


// void guiWindow::on_customPinsEnabled_stateChanged(int arg1)
// {
//     boolSettings[customPins] = arg1;
//     BoxesUpdate();
//     DiffUpdate();
// }

void guiWindow::on_nunChuckToggle_stateChanged(int arg1)
{
    // Update the boolSettings array for nunChuck
    boolSettings[nunChuck] = arg1;

    // Send serial command
    QByteArray command = (arg1 == Qt::Checked) ? "NUNCHUCK\n" : "JOYSTICK\n";  // Add a newline if needed
    qint64 bytesWritten = serialPort.write(command);

    // Debugging output
    if (bytesWritten > 0) {
        qDebug() << "NunChuck support is now:" << command.trimmed();
    } else {
        qWarning() << "Failed to send command to serial port.";
    }

    // Sync changes or trigger further updates
    DiffUpdate();
}



void guiWindow::on_rumbleToggle_stateChanged(int arg1)
{
    boolSettings[rumble] = arg1;
    DiffUpdate();
}


void guiWindow::on_solenoidToggle_stateChanged(int arg1)
{
    boolSettings[solenoid] = arg1;
    DiffUpdate();
}


void guiWindow::on_autofireToggle_stateChanged(int arg1)
{
    boolSettings[autofire] = arg1;
    DiffUpdate();
}


void guiWindow::on_holdToPauseToggle_stateChanged(int arg1)
{
    boolSettings[holdToPause] = arg1;
    DiffUpdate();
}


void guiWindow::on_rumbleIntensityBox_valueChanged(int arg1)
{
    settingsTable[rumbleStrength] = arg1;
    DiffUpdate();
}


void guiWindow::on_rumbleLengthBox_valueChanged(int arg1)
{
    settingsTable[rumbleInterval] = arg1;
    DiffUpdate();
}


void guiWindow::on_holdToPauseLengthBox_valueChanged(int arg1)
{
    settingsTable[holdToPauseLength] = arg1;
    DiffUpdate();
}


void guiWindow::on_solenoidNormalIntervalBox_valueChanged(int arg1)
{
    settingsTable[solenoidNormalInterval] = arg1;
    DiffUpdate();
}


void guiWindow::on_solenoidFastIntervalBox_valueChanged(int arg1)
{
    settingsTable[solenoidFastInterval] = arg1;
    DiffUpdate();
}


void guiWindow::on_solenoidHoldLengthBox_valueChanged(int arg1)
{
    settingsTable[solenoidHoldLength] = arg1;
    DiffUpdate();
}


void guiWindow::on_autofireWaitFactorBox_valueChanged(int arg1)
{
    settingsTable[autofireWaitFactor] = arg1;
    DiffUpdate();
}

void guiWindow::selectedProfile_isChecked(bool isChecked)
{
    // apparently we get two signals at once? So just filter for the on.
    if(isChecked && !serialActive) {
        // Demultiplexing to figure out which "pin" this combobox that's calling correlates to.
        uint8_t slot;
        QObject* obj = sender();
        for(uint8_t i = 0;;i++) {
            if(obj == selectedProfile[i]) {
                slot = i;
                break;
            }
        }
        if(slot != board.selectedProfile) {
            serialPort.write(QString("XC%1").arg(slot+1).toLocal8Bit());
            board.selectedProfile = slot;
            DiffUpdate();
        }
    }
}


void guiWindow::on_calib1Btn_clicked()
{
    serialPort.write("XC1C");
    if (serialPort.waitForBytesWritten(1000)) {
        QLabel *imageLabel = new QLabel(this); // Create QLabel to hold the image
        QPixmap pixmap(":/images/Calibration.png"); // Load the image

        // Scale the image to the size of the window or screen
        pixmap = pixmap.scaled(this->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);

        imageLabel->setPixmap(pixmap); // Set the scaled image to the label
        imageLabel->setAlignment(Qt::AlignCenter); // Center the image in the label
        imageLabel->setGeometry(0, 0, this->width(), this->height()); // Set label to cover the entire window

        // Create the close button
        QPushButton *closeButton = new QPushButton("Close", this);
        closeButton->setGeometry(this->width() - 100, this->height() - 50, 80, 40); // Position the button in the bottom right corner
        closeButton->setStyleSheet("font-size: 14px; background-color: red; color: white;"); // Optional styling

        // Connect the close button click to close the image window
        connect(closeButton, &QPushButton::clicked, [imageLabel, closeButton]() {
            imageLabel->close(); // Close the image window
            closeButton->close(); // Close the close button
        });

        imageLabel->show(); // Show the label containing the image
        closeButton->show(); // Show the close button
    }
}



void guiWindow::on_calib2Btn_clicked()
{
    serialPort.write("XC1C");
    if (serialPort.waitForBytesWritten(1000)) {
        QLabel *imageLabel = new QLabel(this); // Create QLabel to hold the image
        QPixmap pixmap(":/images/Calibration.png"); // Load the image

        // Scale the image to the size of the window or screen
        pixmap = pixmap.scaled(this->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);

        imageLabel->setPixmap(pixmap); // Set the scaled image to the label
        imageLabel->setAlignment(Qt::AlignCenter); // Center the image in the label
        imageLabel->setGeometry(0, 0, this->width(), this->height()); // Set label to cover the entire window

        // Create the close button
        QPushButton *closeButton = new QPushButton("Close", this);
        closeButton->setGeometry(this->width() - 100, this->height() - 50, 80, 40); // Position the button in the bottom right corner
        closeButton->setStyleSheet("font-size: 14px; background-color: red; color: white;"); // Optional styling

        // Connect the close button click to close the image window
        connect(closeButton, &QPushButton::clicked, [imageLabel, closeButton]() {
            imageLabel->close(); // Close the image window
            closeButton->close(); // Close the close button
        });

        imageLabel->show(); // Show the label containing the image
        closeButton->show(); // Show the close button
    }
}


void guiWindow::on_calib3Btn_clicked()
{
    serialPort.write("XC1C");
    if (serialPort.waitForBytesWritten(1000)) {
        QLabel *imageLabel = new QLabel(this); // Create QLabel to hold the image
        QPixmap pixmap(":/images/icons/Calibrate3.png"); // Load the image

        // Scale the image to the size of the window or screen
        pixmap = pixmap.scaled(this->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);

        imageLabel->setPixmap(pixmap); // Set the scaled image to the label
        imageLabel->setAlignment(Qt::AlignCenter); // Center the image in the label
        imageLabel->setGeometry(0, 0, this->width(), this->height()); // Set label to cover the entire window

        // Create the close button
        QPushButton *closeButton = new QPushButton("Close", this);
        closeButton->setGeometry(this->width() - 100, this->height() - 50, 80, 40); // Position the button in the bottom right corner
        closeButton->setStyleSheet("font-size: 14px; background-color: red; color: white;"); // Optional styling

        // Connect the close button click to close the image window
        connect(closeButton, &QPushButton::clicked, [imageLabel, closeButton]() {
            imageLabel->close(); // Close the image window
            closeButton->close(); // Close the close button
        });

        imageLabel->show(); // Show the label containing the image
        closeButton->show(); // Show the close button
    }
}


void guiWindow::on_calib4Btn_clicked()
{
    serialPort.write("XC1C");
    if (serialPort.waitForBytesWritten(1000)) {
        QLabel *imageLabel = new QLabel(this); // Create QLabel to hold the image
        QPixmap pixmap(":/images/icons/Calibrate4.png"); // Load the image

        // Scale the image to the size of the window or screen
        pixmap = pixmap.scaled(this->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);

        imageLabel->setPixmap(pixmap); // Set the scaled image to the label
        imageLabel->setAlignment(Qt::AlignCenter); // Center the image in the label
        imageLabel->setGeometry(0, 0, this->width(), this->height()); // Set label to cover the entire window

        // Create the close button
        QPushButton *closeButton = new QPushButton("Close", this);
        closeButton->setGeometry(this->width() - 100, this->height() - 50, 80, 40); // Position the button in the bottom right corner
        closeButton->setStyleSheet("font-size: 14px; background-color: red; color: white;"); // Optional styling

        // Connect the close button click to close the image window
        connect(closeButton, &QPushButton::clicked, [imageLabel, closeButton]() {
            imageLabel->close(); // Close the image window
            closeButton->close(); // Close the close button
        });

        imageLabel->show(); // Show the label containing the image
        closeButton->show(); // Show the close button
    }
}


// WARNING: make sure "serialActive" is set ON for important operations, or this will eat the fucker
void guiWindow::serialPort_readyRead()
{
    if(!serialActive) {
        while(!serialPort.atEnd()) {
            QString idleBuffer = serialPort.readLine();
            if(idleBuffer.contains("Pressed:")) {
                idleBuffer = idleBuffer.right(4);
                idleBuffer = idleBuffer.trimmed();
                uint8_t button = idleBuffer.toInt();
                switch(button) {
                case btnTrigger:
                    if (!isButtonPressed) {
                        // Load the "clicked" image
                        QPixmap clickedPixmap(":/images/icons/Trigger-Clicked.png");

                        // Scale the pixmap to 115x115
                        clickedPixmap = clickedPixmap.scaled(115, 115, Qt::KeepAspectRatio, Qt::SmoothTransformation);

                        // Set the pixmap to the label
                        ui->btnTriggerLabel->setPixmap(clickedPixmap);

                        // Mark the button as pressed
                        isButtonPressed = true;
                    }
                    break;
                case btnGunA:
                    if (!isButtonPressed) {
                        // Load the "clicked" image
                        QPixmap clickedPixmap(":/images/icons/A-Clicked.png");

                        // Scale the pixmap to 115x115
                        clickedPixmap = clickedPixmap.scaled(115, 115, Qt::KeepAspectRatio, Qt::SmoothTransformation);

                        // Set the pixmap to the label
                        ui->btnALabel->setPixmap(clickedPixmap);

                        // Mark the button as pressed
                        isButtonPressed = true;
                    }
                    break;
                case btnGunB:
                    if (!isButtonPressed) {
                        // Load the "clicked" image
                        QPixmap clickedPixmap(":/images/icons/B-Clicked.png");

                        // Scale the pixmap to 115x115
                        clickedPixmap = clickedPixmap.scaled(115, 115, Qt::KeepAspectRatio, Qt::SmoothTransformation);

                        // Set the pixmap to the label
                        ui->btnBLabel->setPixmap(clickedPixmap);

                        // Mark the button as pressed
                        isButtonPressed = true;
                    }
                    break;
                case btnGunC:
                    if (!isButtonPressed) {
                        // Load the "clicked" image
                        QPixmap clickedPixmap(":/images/icons/C-Clicked.png");

                        // Scale the pixmap to 115x115
                        clickedPixmap = clickedPixmap.scaled(115, 115, Qt::KeepAspectRatio, Qt::SmoothTransformation);

                        // Set the pixmap to the label
                        ui->btnCLabel->setPixmap(clickedPixmap);

                        // Mark the button as pressed
                        isButtonPressed = true;
                    }
                    break;
                case btnStart:
                    if (!isButtonPressed) {
                        // Load the "clicked" image
                        QPixmap clickedPixmap(":/images/icons/Start-Clicked.png");

                        // Scale the pixmap to 115x115
                        clickedPixmap = clickedPixmap.scaled(115, 115, Qt::KeepAspectRatio, Qt::SmoothTransformation);

                        // Set the pixmap to the label
                        ui->btnStartLabel->setPixmap(clickedPixmap);

                        // Mark the button as pressed
                        isButtonPressed = true;
                    }
                    break;
                case btnSelect:
                    if (!isButtonPressed) {
                        // Load the "clicked" image
                        QPixmap clickedPixmap(":/images/icons/Select-Clicked.png");

                        // Scale the pixmap to 115x115
                        clickedPixmap = clickedPixmap.scaled(115, 115, Qt::KeepAspectRatio, Qt::SmoothTransformation);

                        // Set the pixmap to the label
                        ui->btnSelectLabel->setPixmap(clickedPixmap);

                        // Mark the button as pressed
                        isButtonPressed = true;
                    }
                    break;
                case btnGunUp:
                    if (!isButtonPressed) {
                        // Load the "clicked" image
                        QPixmap clickedPixmap(":/images/icons/Up-Clicked.png");

                        // Scale the pixmap to 115x115
                        clickedPixmap = clickedPixmap.scaled(115, 115, Qt::KeepAspectRatio, Qt::SmoothTransformation);

                        // Set the pixmap to the label
                        ui->btnGunUpLabel->setPixmap(clickedPixmap);

                        // Mark the button as pressed
                        isButtonPressed = true;
                    }
                    break;
                case btnGunDown:
                    if (!isButtonPressed) {
                        // Load the "clicked" image
                        QPixmap clickedPixmap(":/images/icons/Down-Clicked.png");

                        // Scale the pixmap to 115x115
                        clickedPixmap = clickedPixmap.scaled(115, 115, Qt::KeepAspectRatio, Qt::SmoothTransformation);

                        // Set the pixmap to the label
                        ui->btnGunDownLabel->setPixmap(clickedPixmap);

                        // Mark the button as pressed
                        isButtonPressed = true;
                    }
                    break;
                case btnGunLeft:
                    if (!isButtonPressed) {
                        // Load the "clicked" image
                        QPixmap clickedPixmap(":/images/icons/Left-Clicked.png");

                        // Scale the pixmap to 115x115
                        clickedPixmap = clickedPixmap.scaled(115, 115, Qt::KeepAspectRatio, Qt::SmoothTransformation);

                        // Set the pixmap to the label
                        ui->btnGunLeftLabel->setPixmap(clickedPixmap);

                        // Mark the button as pressed
                        isButtonPressed = true;
                    }
                    break;
                case btnGunRight:
                    if (!isButtonPressed) {
                        // Load the "clicked" image
                        QPixmap clickedPixmap(":/images/icons/Right-Clicked.png");

                        // Scale the pixmap to 115x115
                        clickedPixmap = clickedPixmap.scaled(115, 115, Qt::KeepAspectRatio, Qt::SmoothTransformation);

                        // Set the pixmap to the label
                        ui->btnGunRightLabel->setPixmap(clickedPixmap);

                        // Mark the button as pressed
                        isButtonPressed = true;
                    }
                    break;
                case btnPedal:
                    if (!isButtonPressed) {
                        // Load the "clicked" image
                        QPixmap clickedPixmap(":/images/icons/Pedal-Clicked.png");

                        // Scale the pixmap to 115x115
                        clickedPixmap = clickedPixmap.scaled(115, 115, Qt::KeepAspectRatio, Qt::SmoothTransformation);

                        // Set the pixmap to the label
                        ui->btnPedalLabel->setPixmap(clickedPixmap);

                        // Mark the button as pressed
                        isButtonPressed = true;
                    }
                    break;
                case btnPump:
                    if (!isButtonPressed) {
                        // Load the "clicked" image
                        QPixmap clickedPixmap(":/images/icons/Pump-Clicked.png");

                        // Scale the pixmap to 115x115
                        clickedPixmap = clickedPixmap.scaled(115, 115, Qt::KeepAspectRatio, Qt::SmoothTransformation);

                        // Set the pixmap to the label
                        ui->btnPumpLabel->setPixmap(clickedPixmap);

                        // Mark the button as pressed
                        isButtonPressed = true;
                    }
                    break;
                }
            } else if(idleBuffer.contains("Released:")) {
                idleBuffer = idleBuffer.right(4);
                idleBuffer = idleBuffer.trimmed();
                uint8_t button = idleBuffer.toInt();
                switch(button) {
                case btnTrigger:
                    if (isButtonPressed) {
                        // Revert to the "default" image on button release
                        QPixmap defaultPixmap(":/images/icons/Trigger.png");


                        // Use the fixed original size of the label to scale the pixmap
                        QSize labelSize(115, 115); // Replace with the actual fixed size of the label

                        // Scale the pixmap to fit the fixed label size while keeping the aspect ratio
                        ui->btnTriggerLabel->setPixmap(defaultPixmap.scaled(labelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));

                        // Set the flag to false, indicating the button has been released
                        isButtonPressed = false;
                    }
                    break;
                case btnGunA:
                    if (isButtonPressed) {
                        // Revert to the "default" image on button release
                        QPixmap defaultPixmap(":/images/icons/T_A_Key_Vintage.png");


                        // Use the fixed original size of the label to scale the pixmap
                        QSize labelSize(115, 115); // Replace with the actual fixed size of the label

                        // Scale the pixmap to fit the fixed label size while keeping the aspect ratio
                        ui->btnALabel->setPixmap(defaultPixmap.scaled(labelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));

                        // Set the flag to false, indicating the button has been released
                        isButtonPressed = false;
                    }
                    break;
                case btnGunB:
                    if (isButtonPressed) {
                        // Revert to the "default" image on button release
                        QPixmap defaultPixmap(":/images/icons/T_B_Key_Vintage.png");


                        // Use the fixed original size of the label to scale the pixmap
                        QSize labelSize(115, 115); // Replace with the actual fixed size of the label

                        // Scale the pixmap to fit the fixed label size while keeping the aspect ratio
                        ui->btnBLabel->setPixmap(defaultPixmap.scaled(labelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));

                        // Set the flag to false, indicating the button has been released
                        isButtonPressed = false;
                    }
                    break;
                case btnGunC:
                    if (isButtonPressed) {
                        // Revert to the "default" image on button release
                        QPixmap defaultPixmap(":/images/icons/T_C_Key_Vintage.png");


                        // Use the fixed original size of the label to scale the pixmap
                        QSize labelSize(115, 115); // Replace with the actual fixed size of the label

                        // Scale the pixmap to fit the fixed label size while keeping the aspect ratio
                        ui->btnCLabel->setPixmap(defaultPixmap.scaled(labelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));

                        // Set the flag to false, indicating the button has been released
                        isButtonPressed = false;
                    }
                    break;
                case btnStart:
                    if (isButtonPressed) {
                        // Revert to the "default" image on button release
                        QPixmap defaultPixmap(":/images/icons/Start.png");


                        // Use the fixed original size of the label to scale the pixmap
                        QSize labelSize(115, 115); // Replace with the actual fixed size of the label

                        // Scale the pixmap to fit the fixed label size while keeping the aspect ratio
                        ui->btnStartLabel->setPixmap(defaultPixmap.scaled(labelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));

                        // Set the flag to false, indicating the button has been released
                        isButtonPressed = false;
                    }
                    break;
                case btnSelect:
                    if (isButtonPressed) {
                        // Revert to the "default" image on button release
                        QPixmap defaultPixmap(":/images/icons/Select.png");


                        // Use the fixed original size of the label to scale the pixmap
                        QSize labelSize(115, 115); // Replace with the actual fixed size of the label

                        // Scale the pixmap to fit the fixed label size while keeping the aspect ratio
                        ui->btnSelectLabel->setPixmap(defaultPixmap.scaled(labelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));

                        // Set the flag to false, indicating the button has been released
                        isButtonPressed = false;
                    }
                    break;
                case btnGunUp:
                    if (isButtonPressed) {
                        // Revert to the "default" image on button release
                        QPixmap defaultPixmap(":/images/icons/T_Up_Key_Vintage.png");


                        // Use the fixed original size of the label to scale the pixmap
                        QSize labelSize(115, 115); // Replace with the actual fixed size of the label

                        // Scale the pixmap to fit the fixed label size while keeping the aspect ratio
                        ui->btnGunUpLabel->setPixmap(defaultPixmap.scaled(labelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));

                        // Set the flag to false, indicating the button has been released
                        isButtonPressed = false;
                    }
                    break;
                case btnGunDown:
                    if (isButtonPressed) {
                        // Revert to the "default" image on button release
                        QPixmap defaultPixmap(":/images/icons/T_Down_Key_Vintage.png");


                        // Use the fixed original size of the label to scale the pixmap
                        QSize labelSize(115, 115); // Replace with the actual fixed size of the label

                        // Scale the pixmap to fit the fixed label size while keeping the aspect ratio
                        ui->btnGunDownLabel->setPixmap(defaultPixmap.scaled(labelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));

                        // Set the flag to false, indicating the button has been released
                        isButtonPressed = false;
                    }
                    break;
                case btnGunLeft:
                    if (isButtonPressed) {
                        // Revert to the "default" image on button release
                        QPixmap defaultPixmap(":/images/icons/T_Left_Key_Vintage.png");


                        // Use the fixed original size of the label to scale the pixmap
                        QSize labelSize(115, 115); // Replace with the actual fixed size of the label

                        // Scale the pixmap to fit the fixed label size while keeping the aspect ratio
                        ui->btnGunLeftLabel->setPixmap(defaultPixmap.scaled(labelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));

                        // Set the flag to false, indicating the button has been released
                        isButtonPressed = false;
                    }
                    break;
                case btnGunRight:
                    if (isButtonPressed) {
                        // Revert to the "default" image on button release
                        QPixmap defaultPixmap(":/images/icons/T_Right_Key_Vintage.png");


                        // Use the fixed original size of the label to scale the pixmap
                        QSize labelSize(115, 115); // Replace with the actual fixed size of the label

                        // Scale the pixmap to fit the fixed label size while keeping the aspect ratio
                        ui->btnGunRightLabel->setPixmap(defaultPixmap.scaled(labelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));

                        // Set the flag to false, indicating the button has been released
                        isButtonPressed = false;
                    }
                    break;
                case btnPedal:
                    if (isButtonPressed) {
                        // Revert to the "default" image on button release
                        QPixmap defaultPixmap(":/images/icons/Pedal.png");


                        // Use the fixed original size of the label to scale the pixmap
                        QSize labelSize(115, 115); // Replace with the actual fixed size of the label

                        // Scale the pixmap to fit the fixed label size while keeping the aspect ratio
                        ui->btnPedalLabel->setPixmap(defaultPixmap.scaled(labelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));

                        // Set the flag to false, indicating the button has been released
                        isButtonPressed = false;
                    }
                    break;
                case btnPump:
                    if (isButtonPressed) {
                        // Revert to the "default" image on button release
                        QPixmap defaultPixmap(":/images/icons/Pump.png");


                        // Use the fixed original size of the label to scale the pixmap
                        QSize labelSize(115, 115); // Replace with the actual fixed size of the label

                        // Scale the pixmap to fit the fixed label size while keeping the aspect ratio
                        ui->btnPumpLabel->setPixmap(defaultPixmap.scaled(labelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));

                        // Set the flag to false, indicating the button has been released
                        isButtonPressed = false;
                    }
                    break;
                }
            } else if(idleBuffer.contains("Profile: ")) {
                idleBuffer = idleBuffer.right(3);
                idleBuffer = idleBuffer.trimmed();
                uint8_t selection = idleBuffer.toInt();
                if(selection != board.selectedProfile) {
                    board.selectedProfile = selection;
                    selectedProfile[selection]->setChecked(true);
                }
                DiffUpdate();
            } else if(idleBuffer.contains("UpdatedProf: ")) {
                idleBuffer = idleBuffer.right(3);
                idleBuffer = idleBuffer.trimmed();
                uint8_t selection = idleBuffer.toInt();
                if(selection != board.selectedProfile) {
                    selectedProfile[selection]->setChecked(true);
                }
                board.selectedProfile = selection;
                idleBuffer = serialPort.readLine();
                xScale[selection]->setText(idleBuffer.trimmed());
                profilesTable[selection].xScale = xScale[selection]->text().toInt();
                idleBuffer = serialPort.readLine();
                yScale[selection]->setText(idleBuffer.trimmed());
                profilesTable[selection].yScale = yScale[selection]->text().toInt();
                idleBuffer = serialPort.readLine();
                xCenter[selection]->setText(idleBuffer.trimmed());
                profilesTable[selection].xCenter = xCenter[selection]->text().toInt();
                idleBuffer = serialPort.readLine();
                yCenter[selection]->setText(idleBuffer.trimmed());
                profilesTable[selection].yCenter = yCenter[selection]->text().toInt();
                DiffUpdate();
            }
        }
    } else if(testMode) {
        QString testBuffer = serialPort.readLine();
        if(testBuffer.contains(',')) {
            QStringList coordsList = testBuffer.remove("\r\n").split(',', Qt::SkipEmptyParts);

            testPointTL.setRect(coordsList[0].toInt()-25, coordsList[1].toInt()-25, 50, 50);
            testPointTR.setRect(coordsList[2].toInt()-25, coordsList[3].toInt()-25, 50, 50);
            testPointBL.setRect(coordsList[4].toInt()-25, coordsList[5].toInt()-25, 50, 50);
            testPointBR.setRect(coordsList[6].toInt()-25, coordsList[7].toInt()-25, 50, 50);
            testPointMed.setRect(coordsList[8].toInt()-25,coordsList[9].toInt()-25, 50, 50);
            testPointD.setRect(coordsList[10].toInt()-25, coordsList[11].toInt()-25, 50, 50);

            QPolygonF poly;
            poly << QPointF(coordsList[0].toInt(), coordsList[1].toInt()) << QPointF(coordsList[2].toInt(), coordsList[3].toInt()) << QPointF(coordsList[6].toInt(), coordsList[7].toInt()) << QPointF(coordsList[4].toInt(), coordsList[5].toInt()) << QPointF(coordsList[0].toInt(), coordsList[1].toInt());
            testBox.setPolygon(poly);
        }
    }
}


void guiWindow::on_rumbleTestBtn_clicked()
{
    serialPort.write("Xtr");
    if(!serialPort.waitForBytesWritten(1000)) {
        PopupWindow("Lost connection to LightGun", "Check your connection & Restart GUI", "Connection Error", 3);
    } else {
        ui->statusBar->showMessage("Sent a rumble test pulse to LightGun.", 2500);
    }
}


void guiWindow::on_solenoidTestBtn_clicked()
{
    serialPort.write("Xts");
    if(!serialPort.waitForBytesWritten(1000)) {
        PopupWindow("Lost connection to LightGun", "Check your connection & Restart GUI", "Connection Error", 3);
    } else {
        ui->statusBar->showMessage("Sent a solenoid test pulse to LightGun.", 2500);
    }
}



void guiWindow::on_testBtn_clicked()
{
    if(serialPort.isOpen()) {
        // Pre-emptively put a sock in the readyRead signal
        serialActive = true;
        serialPort.write("XT");
        serialPort.waitForBytesWritten(1000);
        serialPort.waitForReadyRead(1000);
        if(serialPort.readLine().trimmed() == "Entering Test Mode...") {
            testMode = true;
            ui->testView->setEnabled(true);
            ui->buttonsTestArea->setEnabled(false);
            ui->testBtn->setText("Disable IR Test Mode");
            ui->confirmButton->setEnabled(false);
            ui->confirmButton->setText("[Disabled while in Test Mode]");
            // ui->pinsTab->setEnabled(false);
            ui->settingsTab->setEnabled(false);
            ui->profilesTab->setEnabled(false);
            ui->feedbackTestsBox->setEnabled(false);
            ui->dangerZoneBox->setEnabled(false);
        } else {
            testMode = false;
            ui->testView->setEnabled(false);
            ui->buttonsTestArea->setEnabled(true);
            ui->testBtn->setText("Enable IR Test Mode");
            // ui->pinsTab->setEnabled(true);
            ui->settingsTab->setEnabled(true);
            ui->profilesTab->setEnabled(true);
            ui->feedbackTestsBox->setEnabled(true);
            ui->dangerZoneBox->setEnabled(true);
            DiffUpdate();
            serialActive = false;
        }
    }
}


void guiWindow::on_clearEepromBtn_new_clicked()
{
    QMessageBox messageBox;
    messageBox.setText("Really delete saved data?");
    messageBox.setInformativeText("This operation will delete all saved data, including:\n\n - Calibration Profiles\n - Toggles\n - Settings\n\nAre you sure about this?");
    messageBox.setWindowTitle("Delete Confirmation");
    messageBox.setIcon(QMessageBox::Warning);
    messageBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    messageBox.setDefaultButton(QMessageBox::Yes);
    int value = messageBox.exec();
    if(value == QMessageBox::Yes) {
        if(serialPort.isOpen()) {
            serialActive = true;
            // clear the buffer if anything's been sent.
            while(!serialPort.atEnd()) {
                serialPort.readLine();
            }
            serialPort.write("Xc");
            serialPort.waitForBytesWritten(2000);
            if(serialPort.waitForReadyRead(5000)) {
                QString buffer = serialPort.readLine();
                if(buffer.trimmed() == "Cleared! Please reset the board.") {
                    serialPort.write("XE");
                    serialPort.waitForBytesWritten(2000);
                    serialPort.close();
                    serialActive = false;
                    ui->comPortSelector->setCurrentIndex(0);
                    PopupWindow("Cleared storage.", "Please unplug the board and reinsert it into the PC.", "Clear Finished", 1);
                }
            }
        }
    } else {
        //qDebug() << "Clear operation canceled.";
        ui->statusBar->showMessage("Clear operation canceled.", 3000);
    }
}


void guiWindow::on_baudResetBtn_clicked()
{
    // TODO: Does not work for now, for some reason.
    // Seems to be a QT bug? This is nearly identical to Earle's code.
    qDebug() << "Sending reset command.";
    serialActive = true;
    serialPort.close();
// DIRTY HACK: just directly call OS-level apps to do this for us.
#ifdef Q_OS_UNIX
    // stty does this in a neat one-liner and is standard on *nixes
    QProcess *externalProg = new QProcess;
    QStringList args;
    args << "-F" << QString("%1").arg(serialFoundList[ui->comPortSelector->currentIndex()-1].systemLocation()) << "1200";
    externalProg->start("/usr/bin/stty", args);
    // At least on my system, the Bootloader device takes ~7s to appear
    QThread::msleep(7000);
    // Class-ify this function, maybe.
    QString picoPath;
    foreach(const QStorageInfo &storageDevices, QStorageInfo::mountedVolumes()) {
        if(storageDevices.isValid() && storageDevices.isReady() && storageDevices.displayName() == "RPI-RP2") {
            picoPath = storageDevices.device();
            qDebug() << "Found a Pico bootloader!";
            break;
        } else {
            qDebug() << "nope";
        }
    }
    qDebug() << picoPath;
// QFile::copy("file", picoPath+"file");
#endif
#ifdef Q_OS_WIN
    qDebug()<<"WINDOWS";
    // Ooooh, Windows as a mode option that does basically the same!
    QProcess *externalProg = new QProcess;
    QStringList args;
    // args << QString("%1").arg(serialFoundList[ui->comPortSelector->currentIndex()-1].portName()) << "baud=12" << "parity=n" << "data=8" << "stop=1" << "dtr=off";
    // externalProg->start("mode", args);
    QString comPort = serialFoundList[ui->comPortSelector->currentIndex() - 1].portName();
    args << "/C" << "mode" << comPort << "baud=1200" << "parity=n" << "data=8" << "stop=1" << "dtr=off";

    externalProg->start("cmd.exe", args);

    if (!externalProg->waitForStarted()) {
        qDebug() << "Failed to start cmd.exe";
        return;
    }

    if (!externalProg->waitForFinished()) {
        qDebug() << "Failed to finish cmd.exe";
    }

    QString output = externalProg->readAllStandardOutput();
    QString errorOutput = externalProg->readAllStandardError();

    qDebug() << "Output: " << output;
    qDebug() << "Error Output: " << errorOutput;

    externalProg->close();

#else
// The builtin method that currently does not work at all atm. Let's hope to use this soon.
// serialPort.setDataTerminalReady(false);
// qDebug() << serialPort.isDataTerminalReady();
// QThread::msleep(100);
// serialPort.setBaudRate(QSerialPort::Baud1200);
// qDebug() << serialPort.baudRate();
// QThread::msleep(100);
// serialPort.close();
// QThread::msleep(100);
// qDebug() << serialPort.baudRate();
// qDebug() << serialPort.isDataTerminalReady();
// serialPort.open(QIODevice::ReadOnly);
#endif
    ui->statusBar->showMessage("Board reset to bootloader.", 5000);
    ui->comPortSelector->setCurrentIndex(0);
    serialActive = false;
}

void guiWindow::on_actionAbout_IR_PIGS_triggered()
{
    QDialog *about = new QDialog;
    Ui::aboutDialog aboutDialog;
    aboutDialog.setupUi(about);
    about->show();
}


void guiWindow::on_pbTransfer_clicked()
{
    // on_baudResetBtn_clicked();
    QString selectedDrive = ui->cbUsbDev->itemData(ui->cbUsbDev->currentIndex()).toString();

    QString fileName = "Player" + QString::number(ui->cbPlayer->currentIndex() + 1) + ".uf2";
    QString sourcePath = "./uf2/" + fileName;
    QFile sourceFile(sourcePath);
    if (!sourceFile.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, tr("Error"), tr("Failed to open source file"));
        return;
    }

    QString destinationPath = selectedDrive + "/" + fileName;
    if (!QFile::copy(sourcePath, destinationPath)) {
        qDebug()<<"copy error";
    }

    sourceFile.close();

    exit(1);
}

void guiWindow::on_pbRefreshDev_clicked()
{
    ui->cbUsbDev->clear();
    for (const QStorageInfo &storage : QStorageInfo::mountedVolumes()) {
        if (storage.isValid() && storage.isReady() && !storage.isReadOnly()) {
#ifdef Q_OS_UNIX
            if (storage.device().startsWith("/dev/sd")) { // Assuming UNIX-like system
                ui->cbUsbDev->addItem(storage.displayName() + " (" + storage.rootPath() + ")");
                ui->cbUsbDev->setItemData(ui->cbUsbDev->count() - 1, storage.rootPath());
                // qDebug()<<" usb count: " << ui->cbUsbDev->count();
                // qDebug()<<"display name: " << ui->cbUsbDev->itemText(ui->cbUsbDev->count() - 1) << " data: " << ui->cbUsbDev->itemData(ui->cbUsbDev->count() - 1);
            }
#endif

#ifdef Q_OS_WIN
            ui->cbUsbDev->addItem(storage.displayName() + " (" + storage.rootPath() + ")");
            ui->cbUsbDev->setItemData(ui->cbUsbDev->count() - 1, storage.rootPath());
#endif
        }
    }
}

void guiWindow::on_pbReboot_clicked()
{
    on_baudResetBtn_clicked();
}

