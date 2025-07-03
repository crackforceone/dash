#include "bmw_e61.hpp"

#define DEBUG false


bmw_e61::~bmw_e61()
{
    if (this->climate)
        delete this->climate;
    if (this->vehicle)
        delete this->vehicle;
}

bool bmw_e61::init(ICANBus* canbus){
    this->duelClimate=false;
    if (this->arbiter) {
        this->aa_handler = this->arbiter->android_auto().handler;
        this->climate = new Climate(*this->arbiter);
        this->climate->max_fan_speed(7);
        this->climate->setObjectName("Climate");
        this->vehicle = new Vehicle(*this->arbiter);
        this->vehicle->setObjectName("BMW E61");
        this->vehicle->pressure_init("psi", 30);
        //this->vehicle->disable_sensors();
        this->vehicle->rotate(0);

        if(DEBUG)
            this->debug = new DebugWindow(*this->arbiter);
            

        canbus->registerFrameHandler(0x60D, [this](QByteArray payload){this->monitorHeadlightStatus(payload);});
        canbus->registerFrameHandler(0x54B, [this](QByteArray payload){this->updateClimateDisplay(payload);});
        canbus->registerFrameHandler(0x542, [this](QByteArray payload){this->updateTemperatureDisplay(payload);});
        canbus->registerFrameHandler(0x551, [this](QByteArray payload){this->engineUpdate(payload);});
        canbus->registerFrameHandler(0x385, [this](QByteArray payload){this->tpmsUpdate(payload);});
        canbus->registerFrameHandler(0x354, [this](QByteArray payload){this->brakePedalUpdate(payload);});
        canbus->registerFrameHandler(0x002, [this](QByteArray payload){this->steeringWheelUpdate(payload);});

        canbus->registerFrameHandler(0x1C2, [this](QByteArray payload){this->reversesensor(payload);});
        canbus->registerFrameHandler(0x1D6, [this](QByteArray payload){this->mflUpdate(payload);});
        canbus->registerFrameHandler(0x24A, [this](QByteArray payload){this->reverseStateUpdate(payload);});
        E61_LOG(info)<<"loaded successfully";
        return true;
    }
    else{
        E61_LOG(error)<<"Failed to get arbiter";
        return false;
    }
    

}

QList<QWidget *> bmw_e61::widgets()
{
    QList<QWidget *> tabs;
    tabs.append(this->vehicle);
    tabs.append(this->climate);
    if(DEBUG)
        tabs.append(this->debug);
    return tabs;
}


// TPMS
// 385
// THIRD BYTE:
//  Tire Pressure (PSI) * 4
// FOURTH BYTE:
//  Tire Pressure (PSI) * 4
// FIFTH BYTE:
//  Tire Pressure (PSI) * 4
// SIXTH BYTE:
//  Tire Pressure (PSI) * 4
// SEVENTH BYTE:
// |Tire 1 Pressure Valid|Tire 2 Pressure Valid|Tire 3 Pressure Valid|Tire 4 Pressure Valid|unknown|unknown|unknown|unknown

// OTHERS UNKNOWN

void bmw_e61::tpmsUpdate(QByteArray payload){
    if(DEBUG){
        this->debug->tpmsOne->setText(QString::number((uint8_t)payload.at(2)/4));
        this->debug->tpmsTwo->setText(QString::number((uint8_t)payload.at(3)/4));
        this->debug->tpmsThree->setText(QString::number((uint8_t)payload.at(4)/4));
        this->debug->tpmsFour->setText(QString::number((uint8_t)payload.at(5)/4));
    }
    uint8_t newRrPressure = (uint8_t)payload.at(4)/4;
    uint8_t newRlPressure = (uint8_t)payload.at(5)/4;
    uint8_t newFrPressure = (uint8_t)payload.at(2)/4;
    uint8_t newFlPressure = (uint8_t)payload.at(3)/4;

    this->vehicle->pressure(Position::BACK_RIGHT,newRrPressure);
    this->vehicle->pressure(Position::BACK_LEFT, newRlPressure);
    this->vehicle->pressure(Position::FRONT_RIGHT,newFrPressure);
    this->vehicle->pressure(Position::FRONT_LEFT, newFlPressure);

    DASH_LOG(info) << "[CameraPage] Created GStreamer Pipeline of `" << newRrPressure << "`";

}

void bmw_e61::reversesensor(QByteArray payload) {
    // Extract sensor values from payload bytes
    uint8_t br_sensor = (uint8_t)payload.at(0);  // Back Right
    uint8_t bmr_sensor = (uint8_t)payload.at(1); // Back Middle Right
    uint8_t bml_sensor = (uint8_t)payload.at(2); // Back Middle Left
    uint8_t bl_sensor = (uint8_t)payload.at(3);  // Back Left
    uint8_t fr_sensor = (uint8_t)payload.at(4);  // Front Right
    uint8_t fmr_sensor = (uint8_t)payload.at(5); // Front Middle Right
    uint8_t fml_sensor = (uint8_t)payload.at(6); // Front Middle Left
    uint8_t fl_sensor = (uint8_t)payload.at(7);  // Front Left

    if(DEBUG) {
        DASH_LOG(info) << "Raw sensor values: "
                << "FL:" << static_cast<int>(fl_sensor) << " "
                << "FML:" << static_cast<int>(fml_sensor) << " "
                << "FMR:" << static_cast<int>(fmr_sensor) << " "
                << "FR:" << static_cast<int>(fr_sensor) << " | "
                << "BL:" << static_cast<int>(bl_sensor) << " "
                << "BML:" << static_cast<int>(bml_sensor) << " "
                << "BMR:" << static_cast<int>(bmr_sensor) << " "
                << "BR:" << static_cast<int>(br_sensor);
    }

    // Map the sensor values to levels (0-4)
    auto mapFrontSensorValue  = [](uint8_t value) -> uint8_t {
        if (value >= 250) return 0;       // No detection/maximum distance
        else if (value >= 200) return 1;  // Far
        else if (value >= 150) return 2;  // Medium-Far
        else if (value >= 100) return 3;  // Medium-Close 
        else return 4;                    // Close
    };
    auto mapRearSensorValue   = [](uint8_t value) -> uint8_t {
        if (value >= 250) return 0;       // No detection/maximum distance
        else if (value >= 200) return 1;  // Far
        else if (value >= 150) return 2;  // Medium-Far
        else if (value >= 100) return 3;  // Medium-Close
        else if (value >= 50) return 4;  
        else return 5;                    // Close
    };


    // Update vehicle sensors if vehicle pointer is valid
    if (this->vehicle) {
        // Map all values first
        uint8_t fl_mapped = mapFrontSensorValue(fl_sensor);
        uint8_t fml_mapped = mapFrontSensorValue(fml_sensor);
        uint8_t fmr_mapped = mapFrontSensorValue(fmr_sensor);
        uint8_t fr_mapped = mapFrontSensorValue(fr_sensor);
        uint8_t bl_mapped = mapRearSensorValue(bl_sensor);
        uint8_t bml_mapped = mapRearSensorValue(bml_sensor);
        uint8_t bmr_mapped = mapRearSensorValue(bmr_sensor);
        uint8_t br_mapped = mapRearSensorValue(br_sensor);

        if(DEBUG) {
            DASH_LOG(info) << "Mapped sensor values: "
                    << "FL:" << static_cast<int>(fl_mapped) << " "
                    << "FML:" << static_cast<int>(fml_mapped) << " "
                    << "FMR:" << static_cast<int>(fmr_mapped) << " "
                    << "FR:" << static_cast<int>(fr_mapped) << " | "
                    << "BL:" << static_cast<int>(bl_mapped) << " "
                    << "BML:" << static_cast<int>(bml_mapped) << " "
                    << "BMR:" << static_cast<int>(bmr_mapped) << " "
                    << "BR:" << static_cast<int>(br_mapped);
        }

        // Update all sensors. Even zero values need to be sent to clear previous states
        // Update front sensors (max level 4)
        this->vehicle->sensor(Position::FRONT_LEFT, mapFrontSensorValue(fl_sensor));
        this->vehicle->sensor(Position::FRONT_MIDDLE_LEFT, mapFrontSensorValue(fml_sensor));
        this->vehicle->sensor(Position::FRONT_MIDDLE_RIGHT, mapFrontSensorValue(fmr_sensor));
        this->vehicle->sensor(Position::FRONT_RIGHT, mapFrontSensorValue(fr_sensor));

        // Update back sensors (max level 5)
        this->vehicle->sensor(Position::BACK_LEFT, mapRearSensorValue(bl_sensor));
        this->vehicle->sensor(Position::BACK_MIDDLE_LEFT, mapRearSensorValue(bml_sensor));
        this->vehicle->sensor(Position::BACK_MIDDLE_RIGHT, mapRearSensorValue(bmr_sensor));
        this->vehicle->sensor(Position::BACK_RIGHT, mapRearSensorValue(br_sensor));

        if(DEBUG) {
            DASH_LOG(info) << "Sensors updated";
        }

            // Update sensor value text fields
        // Front sensors
        this->vehicle->sensor_text(Position::FRONT_LEFT, QString::number(fl_sensor));
        this->vehicle->sensor_text(Position::FRONT_MIDDLE_LEFT, QString::number(fml_sensor));
        this->vehicle->sensor_text(Position::FRONT_MIDDLE_RIGHT, QString::number(fmr_sensor));
        this->vehicle->sensor_text(Position::FRONT_RIGHT, QString::number(fr_sensor));

        // Back sensors
        this->vehicle->sensor_text(Position::BACK_LEFT, QString::number(bl_sensor));
        this->vehicle->sensor_text(Position::BACK_MIDDLE_LEFT, QString::number(bml_sensor));
        this->vehicle->sensor_text(Position::BACK_MIDDLE_RIGHT, QString::number(bmr_sensor));
        this->vehicle->sensor_text(Position::BACK_RIGHT, QString::number(br_sensor));


    }
}


//130 (CAN ID changed from 551 to 130)
//(A, B, C, D, E, F, G, H)
// D - Bitfield.

//     0xA0 / 160 - Engine off, "On"
//     0x20 / 32 - Engine turning on (500ms)
//     0x00 / 0 - Engine turning on (2500ms)
//     0x80 / 128 - Engine running
//     0x00 / 0 - Engine shutting down (2500ms)
//     0x20 / 32 - Engine off

// Additional states being monitored:
//     0x41, 0x45, 0x55 - Active states
//     0x40, 0x00 - Trigger pause states

void bmw_e61::engineUpdate(QByteArray payload){
    static uint8_t previousState = 0xFF; // Track previous state
    uint8_t currentState = static_cast<uint8_t>(payload.at(3));
    
    // Check for state change from active states (0x41, 0x45, 0x55) to pause states (0x40, 0x00)
    if ((previousState == 0x41 || previousState == 0x45 || previousState == 0x55) &&
        (currentState == 0x40 || currentState == 0x00)) {
        this->aa_handler->injectButtonPress(aasdk::proto::enums::ButtonCode::PAUSE);
    }
    
    // Update previous state for next comparison
    previousState = currentState;
}

//
//Can-ID	Length	DATA Packet HEX	DATA Packet DECIMAL	Register Desctiprion
//24A	2	05 FF	005 255	05 = In reverse gear
//24A	2	06 FF	006 255	06 = Not in reverse gear
//

void bmw_e61::reverseStateUpdate(QByteArray payload) {
  if(payload.at(0) == 0x05 && payload.at(1) == 0xff) {
      if(!reverseState) {
          this->arbiter->set_curr_page(2);
          reverseState = true;
      }
  }
  else if(payload.at(0) == 0x06 && payload.at(1) == 0xff) {
      if(reverseState) {
          this->arbiter->set_curr_page(0); 
          reverseState = false;
          
          // Reset all parking sensors when exiting reverse mode
          if(this->vehicle) {
              // Reset front sensors
              this->vehicle->sensor(Position::FRONT_LEFT, 0);
              this->vehicle->sensor(Position::FRONT_MIDDLE_LEFT, 0);
              this->vehicle->sensor(Position::FRONT_MIDDLE_RIGHT, 0);
              this->vehicle->sensor(Position::FRONT_RIGHT, 0);
              
              // Reset back sensors
              this->vehicle->sensor(Position::BACK_LEFT, 0);
              this->vehicle->sensor(Position::BACK_MIDDLE_LEFT, 0);
              this->vehicle->sensor(Position::BACK_MIDDLE_RIGHT, 0);
              this->vehicle->sensor(Position::BACK_RIGHT, 0);
              
              // Also reset the sensor text if needed
              this->vehicle->sensor_text(Position::FRONT_LEFT, "");
              this->vehicle->sensor_text(Position::FRONT_MIDDLE_LEFT, "");
              this->vehicle->sensor_text(Position::FRONT_MIDDLE_RIGHT, "");
              this->vehicle->sensor_text(Position::FRONT_RIGHT, "");
              
              this->vehicle->sensor_text(Position::BACK_LEFT, "");
              this->vehicle->sensor_text(Position::BACK_MIDDLE_LEFT, "");
              this->vehicle->sensor_text(Position::BACK_MIDDLE_RIGHT, "");
              this->vehicle->sensor_text(Position::BACK_RIGHT, "");
              
              if(DEBUG) {
                  DASH_LOG(info) << "Exiting reverse mode - all sensors reset";
              }
          }
      }
  }
}

/*
aasdk::proto::enums::ButtonCode::PLAY) != buttonCodes.end());
aasdk::proto::enums::ButtonCode::PAUSE) != buttonCodes.end());
aasdk::proto::enums::ButtonCode::TOGGLE_PLAY) != buttonCodes.end());
aasdk::proto::enums::ButtonCode::NEXT) != buttonCodes.end());
aasdk::proto::enums::ButtonCode::PREV) != buttonCodes.end());
aasdk::proto::enums::ButtonCode::HOME) != buttonCodes.end());
aasdk::proto::enums::ButtonCode::PHONE) != buttonCodes.end());
aasdk::proto::enums::ButtonCode::CALL_END) != buttonCodes.end());
aasdk::proto::enums::ButtonCode::MICROPHONE_1) != buttonCodes.end());
aasdk::proto::enums::ButtonCode::LEFT) != buttonCodes.end());
aasdk::proto::enums::ButtonCode::RIGHT) != buttonCodes.end());
aasdk::proto::enums::ButtonCode::UP) != buttonCodes.end());
aasdk::proto::enums::ButtonCode::DOWN) != buttonCodes.end());
aasdk::proto::enums::ButtonCode::SCROLL_WHEEL) != buttonCodes.end());
aasdk::proto::enums::ButtonCode::BACK) != buttonCodes.end());
aasdk::proto::enums::ButtonCode::ENTER) != buttonCodes.end());
*/


//Can-ID	Length	DATA Packet HEX	DATA Packet DECIMAL	Register Desctiprion
//1D6	2	C0 0C	192 012	No keys prerssed (Continual ping)
//1D6	2	C8 0C	200 012	Volume up
//1D6	2	C4 0C	196 012	Volume down
//1D6	2	E0 0C	224 012	Up Button
//1D6	2	D0 0C	208 012	Down Button
//1D6	2	C1 0C	193 012	Telephone Button
//1D6	2	C0 0D	192 013	Voice button
//1D6	2	C0 1C	192 028	Rotate Button
//1D6	2	C0 4C	192 076	Disk Button


void bmw_e61::mflUpdate(QByteArray payload) {
   // Up button - NEXT
   if(payload.at(0) == 0xE0 && payload.at(1) == 0x0C) {
       if(!upButton)
           this->aa_handler->injectButtonPress(aasdk::proto::enums::ButtonCode::NEXT);
       upButton = true;
   }
   else upButton = false;

   // Down button - PREV
   if(payload.at(0) == 0xD0 && payload.at(1) == 0x0C) {
       if(!downButton)
           this->aa_handler->injectButtonPress(aasdk::proto::enums::ButtonCode::PREV);
       downButton = true;
   }
   else downButton = false;

   // Tel button - PHONE
   if(payload.at(0) == 0xC1 && payload.at(1) == 0x0C) {
       if(!telButton)
           this->aa_handler->injectButtonPress(aasdk::proto::enums::ButtonCode::PHONE);
       telButton = true;
   }
   else telButton = false;

   // Voice button - MICROPHONE_1
   if(payload.at(0) == 0xC0 && payload.at(1) == 0x0D) {
       if(!voiceButton)
           this->aa_handler->injectButtonPress(aasdk::proto::enums::ButtonCode::MICROPHONE_1);
       voiceButton = true;
   }
   else voiceButton = false;
}







//002

void bmw_e61::steeringWheelUpdate(QByteArray payload){
    uint16_t rawAngle = payload.at(1);
    rawAngle = rawAngle<<8;
    rawAngle |= payload.at(0);
    int degAngle = 0;
    if(rawAngle>32767) degAngle = -((65535-rawAngle)/10);
    else degAngle = rawAngle/10;
    degAngle = degAngle/16.4;
    this->vehicle->wheel_steer(degAngle);
    //E61_LOG(info)<<"raw: "<<rawAngle<<" deg "<<degAngle;
}

//354
//(A, B, C, D, E, F, G, H)
// G - brake pedal
//     4 - off
//     20 - pressed a bit

void bmw_e61::brakePedalUpdate(QByteArray payload){
    bool brakePedalUpdate = false;
    if((payload.at(6) == 20)) brakePedalUpdate = true;
    // if(brakePedalUpdate != this->brakePedal){
        // this->brakePedal = brakePedalUpdate;
        this->vehicle->taillights(brakePedalUpdate);
    // }
   
}

// HEADLIGHTS AND DOORS
// 60D
// FIRST BYTE:
// |unknown|RR_DOOR|RL_DOOR|FR_DOOR|FL_DOOR|SIDE_LIGHTS|HEADLIGHTS|unknown|
// SECOND BYTE:
// |unknown|unknown|unknown|unknown|unknown|left turn signal light|right turn signal light|FOGLIGHTS|
// OTHERS UNKNOWN

void bmw_e61::monitorHeadlightStatus(QByteArray payload){
    if((payload.at(0)>>1) & 1){
        //headlights are ON - turn to dark mode
        if(this->arbiter->theme().mode == Session::Theme::Light){
            this->arbiter->set_mode(Session::Theme::Dark);
        this->vehicle->headlights(true);
        }
    }
    else{
        //headlights are off or not fully on (i.e. sidelights only) - make sure is light mode
        if(this->arbiter->theme().mode == Session::Theme::Dark){
            this->arbiter->set_mode(Session::Theme::Light);
        this->vehicle->headlights(false);
        }
    }
    bool rrDoorUpdate = (payload.at(0) >> 6) & 1;
    bool rlDoorUpdate = (payload.at(0) >> 5) & 1;
    bool frDoorUpdate = (payload.at(0) >> 4) & 1;
    bool flDoorUpdate = (payload.at(0) >> 3) & 1;
    this->vehicle->door(Position::BACK_RIGHT, rrDoorUpdate);
    this->vehicle->door(Position::BACK_LEFT, rlDoorUpdate);
    this->vehicle->door(Position::FRONT_RIGHT, frDoorUpdate);
    this->vehicle->door(Position::FRONT_LEFT, flDoorUpdate);

    bool rTurnUpdate = (payload.at(1)>>6) & 1;
    bool lTurnUpdate = (payload.at(1)>>5) & 1;
    this->vehicle->indicators(Position::LEFT, lTurnUpdate);
    this->vehicle->indicators(Position::RIGHT, rTurnUpdate);
}

// HVAC
// 54B

// FIRST BYTE 
// |unknown|unknown|unknown|unknown|unknown|unknown|unknown|HVAC_OFF|
// SECOND BYTE
// |unknown|unknown|unknown|unknown|unknown|unknown|unknown|unknown|
// THIRD BYTE - MODE
// |unknown|unknown|MODE|MODE|MODE|unknown|unknown|unknown|
//   mode:
//    defrost+leg
//       1 0 0
//    head
//       0 0 1
//    head+feet
//       0 1 0
//    feet
//       0 1 1
//    defrost
//       1 0 1
// FOURTH BYTE
// |unknown|DUEL_CLIMATE_ON|DUEL_CLIMATE_ON|unknown|unknown|unknown|RECIRCULATE_OFF|RECIRCULATE_ON|
// Note both duel climate on bytes toggle to 1 when duel climate is on
// FIFTH BYTE - FAN LEVEL
// |unknown|unknown|FAN_1|FAN_2|FAN_3|unknown|unknown|unknown|
// FAN_1, FAN_2, FAN_3 scale linearly fan 0 (off) -> 7
//
// ALL OTHERS UNKNOWN

bool oldStatus = true;

void bmw_e61::updateClimateDisplay(QByteArray payload){
    duelClimate = (payload.at(3)>>5) & 1;
    bool hvacOff = payload.at(0) & 1;
    if(hvacOff != oldStatus){
        oldStatus = hvacOff;
        if(hvacOff){
            climate->airflow(Airflow::OFF);
            climate->fan_speed(0);
            E61_LOG(info)<<"Climate is off";
            return;
        }
    }
    uint8_t airflow = (payload.at(2) >> 3) & 0b111;
    uint8_t dash_airflow = 0;
    switch(airflow){
        case(1):
            dash_airflow = Airflow::BODY;
            break;
        case(2):
            dash_airflow = Airflow::BODY | Airflow::FEET;
            break;
        case(3):
            dash_airflow = Airflow::FEET;
            break;
        case(4):
            dash_airflow = Airflow::DEFROST | Airflow::FEET;
            break;
        case(5):
            dash_airflow = Airflow::DEFROST;
            break;
    }
    if(climate->airflow()!=dash_airflow)
        climate->airflow(dash_airflow);
    uint8_t fanLevel = (payload.at(4)>>3) & 0b111;
    if(climate->fan_speed()!=fanLevel)
        climate->fan_speed(fanLevel);
}

// Climate
// 542

// FIRST BYTE:
// unknown
// SECOND BYTE:
// entire byte is driver side temperature, 60F->90F
// THIRD BYTE:
// entire byte is passenger side temperature, 60F->90F
// note that this byte is only updated when duel climate is on. When duel climate is off, SECOND BYTE contains accurate passenger temperature.

void bmw_e61::updateTemperatureDisplay(QByteArray payload){
    if(climate->left_temp()!=(unsigned char)payload.at(1))
        climate->left_temp((unsigned char)payload.at(1));
    if(duelClimate){
        if(climate->right_temp()!=(unsigned char)payload.at(2)){
            climate->right_temp((unsigned char)payload.at(2));
        }
    }else{
        if(climate->right_temp()!=(unsigned char)payload.at(1))
            climate->right_temp((unsigned char)payload.at(1));
    }
}




DebugWindow::DebugWindow(Arbiter &arbiter, QWidget *parent) : QWidget(parent)
{
    this->setObjectName("Debug");


    QLabel* textOne = new QLabel("Front Right PSI", this);
    QLabel* textTwo = new QLabel("Front Left PSI", this);
    QLabel* textThree = new QLabel("Rear Right PSI", this);
    QLabel* textFour = new QLabel("Rear Left PSI", this);

    tpmsOne = new QLabel("--", this);
    tpmsTwo = new QLabel("--", this);
    tpmsThree = new QLabel("--", this);
    tpmsFour = new QLabel("--", this);



    QVBoxLayout *layout = new QVBoxLayout(this);

    layout->addWidget(textOne);
    layout->addWidget(tpmsOne);
    layout->addWidget(Session::Forge::br(false));

    layout->addWidget(textTwo);
    layout->addWidget(tpmsTwo);
    layout->addWidget(Session::Forge::br(false));

    layout->addWidget(textThree);
    layout->addWidget(tpmsThree);
    layout->addWidget(Session::Forge::br(false));

    layout->addWidget(textFour); 
    layout->addWidget(tpmsFour);
    layout->addWidget(Session::Forge::br(false));




}