#include "elmo_ethercat_sdk/Elmo.hpp"
#include "elmo_ethercat_sdk/ConfigurationParser.hpp"
#include "elmo_ethercat_sdk/ObjectDictionary.hpp"
#include "elmo_ethercat_sdk/RxPdo.hpp"
#include "elmo_ethercat_sdk/TxPdo.hpp"

#include <cmath>

namespace elmo{

  std::string Elmo::getName() const{
    return name_;
  }

  bool Elmo::startup(){
    return (configureRxPdo(rxPdoTypeEnum_) && configureTxPdo(txPdoTypeEnum_));
  }

  void Elmo::shutdown(){
    bus_->setState(EC_STATE_INIT, address_);
  }

  void Elmo::updateWrite(){
    /* locking the mutex_
    ** This is necessary since "updateWrite" is called from an external thread
    */
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    /*
    ** Check if the Mode of Operation has been set properly
    */
    if (modeOfOperation_ == ModeOfOperationEnum::NA) {
      reading_.addError(ErrorType::ModeOfOperationError);
      MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::updateWrite] Mode of operation for '"
                        << name_ << "' has not been set.");
      return;
    }

    /*!
    * engage the state machine if a state change is requested
    */
    if (conductStateChange_ && hasRead_) {
      engagePdoStateMachine();
    }

    switch (currentRxPdoTypeEnum_) {
      case RxPdoTypeEnum::RxPdoStandard: {
        RxPdoStandard rxPdo{};
        rxPdo.targetPosition_ = stagedCommand_.getTargetPositionRaw();
        rxPdo.targetVelocity_ = stagedCommand_.getTargetVelocityRaw();
        rxPdo.targetTorque_ = stagedCommand_.getTargetTorqueRaw();
        rxPdo.maxTorque_ = stagedCommand_.getMaxTorqueRaw();
        rxPdo.modeOfOperation_ = static_cast<int8_t>(modeOfOperation_);
        rxPdo.torqueOffset_ = stagedCommand_.getTorqueOffsetRaw();
        rxPdo.controlWord_ = controlword_.getRawControlword();

        // actually writing to the hardware
        bus_->writeRxPdo(address_, rxPdo);
      } break;
      case RxPdoTypeEnum::RxPdoCST: {
        RxPdoCST rxPdo{};
        rxPdo.targetTorque_ = stagedCommand_.getTargetTorqueRaw();
        rxPdo.modeOfOperation_ = static_cast<int8_t>(modeOfOperation_);
        rxPdo.controlWord_ = controlword_.getRawControlword();

        // actually writing to the hardware
        bus_->writeRxPdo(address_, rxPdo);
      } break;

      default:
        MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::updateWrite] Unsupported Rx Pdo type for '"
                        << name_ << "'");
        addErrorToReading(ErrorType::RxPdoTypeError);
    }
  }

  void Elmo::updateRead(){
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // TODO(duboisf): implement some sort of time stamp
    switch (currentTxPdoTypeEnum_) {
      case TxPdoTypeEnum::TxPdoStandard: {
        TxPdoStandard txPdo{};
        // reading from the bus
        bus_->readTxPdo(address_, txPdo);
        reading_.setActualPosition(txPdo.actualPosition_);
        reading_.setDigitalInputs(txPdo.digitalInputs_);
        reading_.setActualVelocity(txPdo.actualVelocity_);
        reading_.setStatusword(txPdo.statusword_);
        reading_.setAnalogInput(txPdo.analogInput_);
        reading_.setActualCurrent(txPdo.actualCurrent_);
        reading_.setBusVoltage(txPdo.busVoltage_);
      } break;
      case TxPdoTypeEnum::TxPdoCST: {
        TxPdoCST txPdo{};
        // reading from the bus
        bus_->readTxPdo(address_, txPdo);
        reading_.setActualPosition(txPdo.actualPosition_);
        reading_.setActualCurrent(txPdo.actualTorque_);  /// torque readings are actually current readings,
                                                        /// the conversion is handled later
        reading_.setStatusword(txPdo.statusword_);
        reading_.setActualVelocity(txPdo.actualVelocity_);
      } break;

      default:
        MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::updateRrite] Unsupported Tx Pdo type for '"
                        << name_ << "'");
        reading_.addError(ErrorType::TxPdoTypeError);
    }

    // set the hasRead_ variable to true since a nes reading was read
    if (!hasRead_) {
      hasRead_ = true;
    }

    /*!
    * Check whether the state has changed to "FAULT"
    */
    if (reading_.getDriveState() == DriveState::Fault) {
      uint16_t fault;
      if (sendSdoReadUInt16(OD_INDEX_ERROR_CODE, 0, false, fault)) {  // TODO(duboisf) check
        reading_.addFault(fault);
      } else {
        reading_.addError(ErrorType::ErrorReadingError);
      }
    }
  }

  bool Elmo::runPreopConfiguration(){
    bool success = true;
    // motor rated current not specified, load hardware value over EtherCAT (SDO)
    if(configuration_.motorRatedCurrentA == 0.0){
      uint32_t motorRatedCurrent;
      success &= sendSdoRead(OD_INDEX_MOTOR_RATED_CURRENT, 0, false, motorRatedCurrent);
      configuration_.motorRatedCurrentA = static_cast<double>(motorRatedCurrent)/1000.0 ;
      reading_.configureReading(configuration_);
    }

    success &= setDriveStateViaSdo(DriveState::ReadyToSwitchOn);
    // PDO mapping
    success &= mapPdos(configuration_.rxPdoTypeEnum, configuration_.txPdoTypeEnum);
    // Set initial mode of operation
    success &= sdoVerifyWrite(OD_INDEX_MODES_OF_OPERATION,
                              0,
                              false, static_cast<int8_t>(configuration_.modeOfOperationEnum),
                              configuration_.configRunSdoVerifyTimeout);
    // To be on the safe side: set currect PDO sizes
    autoConfigurePdoSizes();

    /*
    ** Write requested motor rated current to the drives.
    ** The motor rated torque is set to the same value since we handle
    ** the current / torque conversion in this library and not on
    ** the hardware.
    */
    uint32_t motorRatedCurrent = static_cast<uint32_t>(
      round(1000.0 * configuration_.motorRatedCurrentA));
    success &= sdoVerifyWrite(OD_INDEX_MOTOR_RATED_CURRENT, 0, false, motorRatedCurrent);
    success &= sdoVerifyWrite(OD_INDEX_MOTOR_RATED_TORQUE, 0, false, motorRatedCurrent);

    // Write maximum current to drive
    uint16_t maxCurrent = static_cast<uint16_t>(floor(1000.0 * configuration_.maxCurrentA));
    success &= sdoVerifyWrite(OD_INDEX_MAX_CURRENT, 0, false, maxCurrent);

    if(!success){
      MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::runPreopConfiguration] hardware configuration of '"
                        << name_ <<"' not successful!");
      addErrorToReading(ErrorType::ConfigurationError);
    }
    return success;
  }

  void Elmo::stageCommand(const Command& command){
    std::lock_guard<std::recursive_mutex> lock(stagedCommandMutex_);
    stagedCommand_ = command;
    stagedCommand_.setPositionFactorRadToInteger(
      static_cast<double>(configuration_.positionEncoderResolution) / (2.0 * M_PI));
    stagedCommand_.setVelocityFactorRadPerSecToIntegerPerSec(
      static_cast<double>(configuration_.positionEncoderResolution) / (2.0 * M_PI));

    double currentFactorAToInt = 1000.0 / configuration_.motorRatedCurrentA;
    stagedCommand_.setCurrentFactorAToInteger(currentFactorAToInt);
    stagedCommand_.setTorqueFactorNmToInteger(
      currentFactorAToInt / configuration_.motorConstant / configuration_.gearRatio);

    stagedCommand_.setMaxCurrent(configuration_.maxCurrentA);
    stagedCommand_.setMaxTorque(
      configuration_.maxCurrentA * configuration_.motorConstant * configuration_.gearRatio);

    stagedCommand_.setUseRawCommands(configuration_.useRawCommands);

    stagedCommand_.doUnitConversion();

    if(allowModeChange_){
      modeOfOperation_ = command.getModeOfOperation();
    }

  }

  Reading Elmo::getReading() const{
    std::lock_guard<std::recursive_mutex> lock(readingMutex_);
    return reading_;
  }

  void Elmo::getReading(Reading &reading) const{
    std::lock_guard<std::recursive_mutex> lock(readingMutex_);
    reading = reading_;
  }

  bool Elmo::loadConfigFile(const std::string &fileName){
    ConfigurationParser configurationParser(fileName);
    return loadConfiguration(configurationParser.getConfiguration());
  }

  bool Elmo::loadConfigNode(YAML::Node configNode){
    ConfigurationParser configurationParser(configNode);
    return loadConfiguration(configurationParser.getConfiguration());
  }

  bool Elmo::loadConfiguration(const Configuration& configuration){
    bool success = true;
    reading_.configureReading(configuration);

    // Check if changing mode of operation will be allowed
    allowModeChange_ = true;
    allowModeChange_ &= configuration.useMultipleModeOfOperations;
    allowModeChange_ &= (configuration.rxPdoTypeEnum == RxPdoTypeEnum::RxPdoStandard);
    allowModeChange_ &= (configuration.txPdoTypeEnum == TxPdoTypeEnum::TxPdoStandard);

    modeOfOperation_ = configuration.modeOfOperationEnum;

    configuration_ = configuration;
    return success;
  }

  Configuration Elmo::getConfiguration() const{
    return configuration_;
  }

  bool Elmo::getStatuswordViaSdo(Statusword &statusword){
    uint16_t statuswordValue = 0;
    bool success = sendSdoRead(OD_INDEX_STATUSWORD, 0, false, statuswordValue);
    statusword.setFromRawStatusword(statuswordValue);
    return success;
  }

  bool Elmo::setControlwordViaSdo(Controlword &controlword){
    return sendSdoWrite(OD_INDEX_CONTROLWORD, 0, false, controlword.getRawControlword());
  }

  bool Elmo::setDriveStateViaSdo(const DriveState &driveState){
    bool success = true;
    Statusword currentStatusword;
    success &= getStatuswordViaSdo(currentStatusword);
    DriveState currentDriveState = currentStatusword.getDriveState();

    // do the adequate state changes (via sdo) depending on the requested and
    // current drive states
    switch (driveState) {
      // Target: switch on disabled
      // This is the lowest state in which the state machine can be brought over
      // EtherCAT
      case DriveState::SwitchOnDisabled:
        switch (currentDriveState) {
          case DriveState::SwitchOnDisabled:
            success &= true;
            break;
          case DriveState::ReadyToSwitchOn:
            success &= stateTransitionViaSdo(StateTransition::_7);
            break;
          case DriveState::SwitchedOn:
            success &= stateTransitionViaSdo(StateTransition::_10);
            break;
          case DriveState::OperationEnabled:
            success &= stateTransitionViaSdo(StateTransition::_9);
            break;
          case DriveState::QuickStopActive:
            success &= stateTransitionViaSdo(StateTransition::_12);
            break;
          case DriveState::Fault:
            success &= stateTransitionViaSdo(StateTransition::_15);
            break;
          default:
            MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::setDriveStateViaSdo] State Transition not implemented");
            addErrorToReading(ErrorType::SdoStateTransitionError);
            success = false;
        }
        break;

      case DriveState::ReadyToSwitchOn:
        switch (currentDriveState) {
          case DriveState::SwitchOnDisabled:
            success &= stateTransitionViaSdo(StateTransition::_2);
            break;
          case DriveState::ReadyToSwitchOn:
            success &= true;
            break;
          case DriveState::SwitchedOn:
            success &= stateTransitionViaSdo(StateTransition::_6);
            break;
          case DriveState::OperationEnabled:
            success &= stateTransitionViaSdo(StateTransition::_8);
            break;
          case DriveState::QuickStopActive:
            success &= stateTransitionViaSdo(StateTransition::_12);
            success &= stateTransitionViaSdo(StateTransition::_2);
            break;
          case DriveState::Fault:
            success &= stateTransitionViaSdo(StateTransition::_15);
            success &= stateTransitionViaSdo(StateTransition::_2);
            break;
          default:
            MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::setDriveStateViaSdo] State Transition not implemented");
            addErrorToReading(ErrorType::SdoStateTransitionError);
            success = false;
        }
        break;

      case DriveState::SwitchedOn:
        switch (currentDriveState) {
          case DriveState::SwitchOnDisabled:
            success &= stateTransitionViaSdo(StateTransition::_2);
            success &= stateTransitionViaSdo(StateTransition::_3);
            break;
          case DriveState::ReadyToSwitchOn:
            success &= stateTransitionViaSdo(StateTransition::_3);
            break;
          case DriveState::SwitchedOn:
            success &= true;
            break;
          case DriveState::OperationEnabled:
            success &= stateTransitionViaSdo(StateTransition::_5);
            break;
          case DriveState::QuickStopActive:
            success &= stateTransitionViaSdo(StateTransition::_12);
            success &= stateTransitionViaSdo(StateTransition::_2);
            success &= stateTransitionViaSdo(StateTransition::_3);
            break;
          case DriveState::Fault:
            success &= stateTransitionViaSdo(StateTransition::_15);
            success &= stateTransitionViaSdo(StateTransition::_2);
            success &= stateTransitionViaSdo(StateTransition::_3);
            break;
          default:
            MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::setDriveStateViaSdo] State Transition not implemented");
            addErrorToReading(ErrorType::SdoStateTransitionError);
            success = false;
        }
        break;

      case DriveState::OperationEnabled:
        switch (currentDriveState) {
          case DriveState::SwitchOnDisabled:
            success &= stateTransitionViaSdo(StateTransition::_2);
            success &= stateTransitionViaSdo(StateTransition::_3);
            success &= stateTransitionViaSdo(StateTransition::_4);
            break;
          case DriveState::ReadyToSwitchOn:
            success &= stateTransitionViaSdo(StateTransition::_3);
            success &= stateTransitionViaSdo(StateTransition::_4);
            break;
          case DriveState::SwitchedOn:
            success &= stateTransitionViaSdo(StateTransition::_4);
            break;
          case DriveState::OperationEnabled:
            success &= true;
            break;
          case DriveState::QuickStopActive:
            success &= stateTransitionViaSdo(StateTransition::_12);
            success &= stateTransitionViaSdo(StateTransition::_2);
            success &= stateTransitionViaSdo(StateTransition::_3);
            success &= stateTransitionViaSdo(StateTransition::_4);
            break;
          case DriveState::Fault:
            success &= stateTransitionViaSdo(StateTransition::_15);
            success &= stateTransitionViaSdo(StateTransition::_2);
            success &= stateTransitionViaSdo(StateTransition::_3);
            success &= stateTransitionViaSdo(StateTransition::_4);
            break;
          default:
            MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::setDriveStateViaSdo] State Transition not implemented");
            addErrorToReading(ErrorType::SdoStateTransitionError);
            success = false;
        }
        break;

      case DriveState::QuickStopActive:
        switch (currentDriveState) {
          case DriveState::SwitchOnDisabled:
            success &= stateTransitionViaSdo(StateTransition::_2);
            success &= stateTransitionViaSdo(StateTransition::_3);
            success &= stateTransitionViaSdo(StateTransition::_4);
            success &= stateTransitionViaSdo(StateTransition::_11);
            break;
          case DriveState::ReadyToSwitchOn:
            success &= stateTransitionViaSdo(StateTransition::_3);
            success &= stateTransitionViaSdo(StateTransition::_4);
            success &= stateTransitionViaSdo(StateTransition::_11);
            break;
          case DriveState::SwitchedOn:
            success &= stateTransitionViaSdo(StateTransition::_4);
            success &= stateTransitionViaSdo(StateTransition::_11);
            break;
          case DriveState::OperationEnabled:
            success &= stateTransitionViaSdo(StateTransition::_11);
            break;
          case DriveState::QuickStopActive:
            success &= true;
            break;
          case DriveState::Fault:
            success &= stateTransitionViaSdo(StateTransition::_15);
            success &= stateTransitionViaSdo(StateTransition::_2);
            success &= stateTransitionViaSdo(StateTransition::_3);
            success &= stateTransitionViaSdo(StateTransition::_4);
            success &= stateTransitionViaSdo(StateTransition::_11);
            break;
          default:
            MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::setDriveStateViaSdo] State Transition not implemented");
            addErrorToReading(ErrorType::SdoStateTransitionError);
            success = false;
        }
        break;

      default:
        MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::setDriveStateViaSdo] State Transition not implemented");
        addErrorToReading(ErrorType::SdoStateTransitionError);
        success = false;
    }
    return success;
  }

  bool Elmo::stateTransitionViaSdo(const StateTransition& stateTransition){
    Controlword controlword;
    switch (stateTransition) {
      case StateTransition::_2:
        controlword.setStateTransition2();
        return setControlwordViaSdo(controlword);
        break;
      case StateTransition::_3:
        controlword.setStateTransition3();
        return setControlwordViaSdo(controlword);
        break;
      case StateTransition::_4:
        controlword.setStateTransition4();
        return setControlwordViaSdo(controlword);
        break;
      case StateTransition::_5:
        controlword.setStateTransition5();
        return setControlwordViaSdo(controlword);
        break;
      case StateTransition::_6:
        controlword.setStateTransition6();
        return setControlwordViaSdo(controlword);
        break;
      case StateTransition::_7:
        controlword.setStateTransition7();
        return setControlwordViaSdo(controlword);
        break;
      case StateTransition::_8:
        controlword.setStateTransition8();
        return setControlwordViaSdo(controlword);
        break;
      case StateTransition::_9:
        controlword.setStateTransition9();
        return setControlwordViaSdo(controlword);
        break;
      case StateTransition::_10:
        controlword.setStateTransition10();
        return setControlwordViaSdo(controlword);
        break;
      case StateTransition::_11:
        controlword.setStateTransition11();
        return setControlwordViaSdo(controlword);
        break;
      case StateTransition::_12:
        controlword.setStateTransition12();
        return setControlwordViaSdo(controlword);
        break;
      case StateTransition::_15:
        controlword.setStateTransition15();
        return setControlwordViaSdo(controlword);
        break;
      default:
        MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::stateTransitionViaSdo] State Transition not implemented");
        addErrorToReading(ErrorType::SdoStateTransitionError);
        return false;
    }
  }

  bool Elmo::setDriveStateViaPdo(const DriveState &driveState, const bool waitForState){
    bool success = false;
    /*
    ** locking the mutex_
    ** This is not done with a lock_guard here because during the waiting time the
    ** mutex_ must be unlocked periodically such that PDO writing (and thus state
    ** changes) may occur at all!
    */
    mutex_.lock();

    // reset the "stateChangeSuccessful_" flag to false such that a new successful
    // state change can be detected
    stateChangeSuccessful_ = false;

    // make the state machine realize that a state change will have to happen
    conductStateChange_ = true;

    // overwrite the target drive state
    targetDriveState_ = driveState;

    // set the hasRead flag to false such that at least one new reading will be
    // available when starting the state change
    hasRead_ = false;

    // set the time point of the last pdo change to now
    driveStateChangeTimePoint_ = std::chrono::steady_clock::now();

    // set a temporary time point to prevent getting caught in an infinite loop
    auto driveStateChangeStartTimePoint = std::chrono::steady_clock::now();

    // return if no waiting is requested
    if (!waitForState) {
      // unlock the mutex
      mutex_.unlock();
      // return true if no waiting is requested
      return true;
    }

    // Wait for the state change to be successful
    // during the waiting time the mutex MUST be unlocked!

    while (true) {
      // break loop as soon as the state change was successful
      if (stateChangeSuccessful_) {
        success = true;
        break;
      }

      // break the loop if the state change takes too long
      // this prevents a freezing of the end user's program if the hardware is not
      // able to change it's state.
      if ((std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - driveStateChangeStartTimePoint)).count() >
          configuration_.driveStateChangeMaxTimeout) {
        break;
      }
      // unlock the mutex during sleep time
      mutex_.unlock();
      usleep(1000);
      // lock the mutex to be able to check the success flag
      mutex_.lock();
    }
    // unlock the mutex one last time
    mutex_.unlock();
    return success;
  }

  bool Elmo::mapPdos(RxPdoTypeEnum rxPdoTypeEnum, TxPdoTypeEnum txPdoTypeEnum){
    bool rxSuccess = true;
    switch (rxPdoTypeEnum) {
      case RxPdoTypeEnum::RxPdoStandard:
        usleep(configuration_.configRunSdoVerifyTimeout);
        rxSuccess &= sdoVerifyWrite(OD_INDEX_RX_PDO_ASSIGNMENT, 0, false, static_cast<uint8_t>(0), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        rxSuccess &= sdoVerifyWrite(OD_INDEX_RX_PDO_ASSIGNMENT, 1, false, static_cast<uint16_t>(0x1605), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        rxSuccess &= sdoVerifyWrite(OD_INDEX_RX_PDO_ASSIGNMENT, 2, false, static_cast<uint16_t>(0x1618), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        rxSuccess &= sdoVerifyWrite(OD_INDEX_RX_PDO_ASSIGNMENT, 0, false, static_cast<uint8_t>(2), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        break;
      case RxPdoTypeEnum::RxPdoCST:
        usleep(configuration_.configRunSdoVerifyTimeout);
        rxSuccess &= sdoVerifyWrite(OD_INDEX_RX_PDO_ASSIGNMENT, 0, false, static_cast<uint8_t>(0), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        rxSuccess &= sdoVerifyWrite(OD_INDEX_RX_PDO_ASSIGNMENT, 1, false, static_cast<uint16_t>(0x1602), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        rxSuccess &= sdoVerifyWrite(OD_INDEX_RX_PDO_ASSIGNMENT, 2, false, static_cast<uint16_t>(0x160b), configuration_.configRunSdoVerifyTimeout);
        rxSuccess &= sdoVerifyWrite(OD_INDEX_RX_PDO_ASSIGNMENT, 0, false, static_cast<uint8_t>(2), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        break;

      case RxPdoTypeEnum::NA:
        MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::mapPdos] Cannot map RxPdo, PdoType not configured properly");
        addErrorToReading(ErrorType::PdoMappingError);
        rxSuccess = false;
        break;
      default:  // Non-implemented type
        MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::mapPdos] Cannot map RxPdo, PdoType not configured properly");
        addErrorToReading(ErrorType::PdoMappingError);
        rxSuccess = false;
        break;
    }

    bool txSuccess = true;
    switch (txPdoTypeEnum) {
      case TxPdoTypeEnum::TxPdoStandard:
        txSuccess &= sdoVerifyWrite(OD_INDEX_TX_PDO_ASSIGNMENT, 0, false, static_cast<uint8_t>(0), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        txSuccess &= sdoVerifyWrite(OD_INDEX_TX_PDO_ASSIGNMENT, 1, false, static_cast<uint16_t>(0x1a03), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        txSuccess &= sdoVerifyWrite(OD_INDEX_TX_PDO_ASSIGNMENT, 2, false, static_cast<uint16_t>(0x1a1d), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        txSuccess &= sdoVerifyWrite(OD_INDEX_TX_PDO_ASSIGNMENT, 3, false, static_cast<uint16_t>(0x1a1f), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        txSuccess &= sdoVerifyWrite(OD_INDEX_TX_PDO_ASSIGNMENT, 4, false, static_cast<uint16_t>(0x1a18), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        txSuccess &= sdoVerifyWrite(OD_INDEX_TX_PDO_ASSIGNMENT, 0, false, static_cast<uint8_t>(4), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        break;

      case TxPdoTypeEnum::TxPdoCST:
        txSuccess &= sdoVerifyWrite(OD_INDEX_TX_PDO_ASSIGNMENT, 0, false, static_cast<uint8_t>(0), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        txSuccess &= sdoVerifyWrite(OD_INDEX_TX_PDO_ASSIGNMENT, 1, false, static_cast<uint16_t>(0x1a02), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        txSuccess &= sdoVerifyWrite(OD_INDEX_TX_PDO_ASSIGNMENT, 2, false, static_cast<uint16_t>(0x1a11), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        txSuccess &= sdoVerifyWrite(OD_INDEX_TX_PDO_ASSIGNMENT, 0, false, static_cast<uint8_t>(2), configuration_.configRunSdoVerifyTimeout);
        usleep(configuration_.configRunSdoVerifyTimeout);
        break;

      case TxPdoTypeEnum::NA:
        MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::mapPdos] Cannot map TxPdo, PdoType not configured properly");
        addErrorToReading(ErrorType::TxPdoMappingError);
        txSuccess = false;
        break;
      default:  // if any case was forgotten
        MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::mapPdos] Cannot map TxPdo, PdoType not configured properly");
        addErrorToReading(ErrorType::TxPdoMappingError);
        txSuccess = false;
        break;
    }

    if (rxSuccess) {
      rxSuccess &= configureRxPdo(rxPdoTypeEnum);
    }
    if (txSuccess) {
      txSuccess &= configureTxPdo(txPdoTypeEnum);
    }

    return (txSuccess && rxSuccess);
  }

  bool Elmo::configureRxPdo(const RxPdoTypeEnum rxPdoTypeEnum){
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // invalid Type
    if (rxPdoTypeEnum == RxPdoTypeEnum::NA) {
      MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::configureRxPdo] Invalid Rx PDO Type.");
      addErrorToReading(ErrorType::RxPdoTypeError);
      return false;
    }

    // the types already coincide
    if (rxPdoTypeEnum == getCurrentRxPdoTypeEnum()) {
      return true;
    }

    // set the current Pdo type
    else {
      currentRxPdoTypeEnum_ = rxPdoTypeEnum;
      return true;
    }
  }

  bool Elmo::configureTxPdo(const TxPdoTypeEnum txPdoTypeEnum){
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // invalid Type
    if (txPdoTypeEnum == TxPdoTypeEnum::NA) {
      MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::configureTxPdo] Invalid Tx PDO Type.");
      addErrorToReading(ErrorType::RxPdoTypeError);
      return false;
    }

    // the types already coincide
    if (txPdoTypeEnum == getCurrentTxPdoTypeEnum()) {
      return true;
    }

    // set the current Pdo type
    else {
      currentTxPdoTypeEnum_ = txPdoTypeEnum;
      return true;
    }
  }

  Controlword Elmo::getNextStateTransitionControlword(const DriveState& requestedDriveState,
                                                      const DriveState& currentDriveState){
    Controlword controlword;
    controlword.setAllFalse();
    switch (requestedDriveState) {
      case DriveState::SwitchOnDisabled:
        switch (currentDriveState) {
          case DriveState::SwitchOnDisabled:
            MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::getNextStateTransitionControlword] "
                              << "drive state has already been reached for '"
                              << name_ << "'");
            addErrorToReading(ErrorType::PdoStateTransitionError);
            break;
          case DriveState::ReadyToSwitchOn:
            controlword.setStateTransition7();
            break;
          case DriveState::SwitchedOn:
            controlword.setStateTransition10();
            break;
          case DriveState::OperationEnabled:
            controlword.setStateTransition9();
            break;
          case DriveState::QuickStopActive:
            controlword.setStateTransition12();
            break;
          case DriveState::Fault:
            controlword.setStateTransition15();
            break;
          default:
            MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::getNextStateTransitionControlword] "
                              << "PDO state transition not implemented for '"
                              << name_ << "'");
            addErrorToReading(ErrorType::PdoStateTransitionError);
        }
        break;

      case DriveState::ReadyToSwitchOn:
        switch (currentDriveState) {
          case DriveState::SwitchOnDisabled:
            controlword.setStateTransition2();
            break;
          case DriveState::ReadyToSwitchOn:
            MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::getNextStateTransitionControlword] "
                              << "drive state has already been reached for '"
                              << name_ << "'");
            addErrorToReading(ErrorType::PdoStateTransitionError);
            break;
          case DriveState::SwitchedOn:
            controlword.setStateTransition6();
            break;
          case DriveState::OperationEnabled:
            controlword.setStateTransition8();
            break;
          case DriveState::QuickStopActive:
            controlword.setStateTransition12();
            break;
          case DriveState::Fault:
            controlword.setStateTransition15();
            break;
          default:
            MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::getNextStateTransitionControlword] "
                              << "PDO state transition not implemented for '"
                              << name_ << "'");
            addErrorToReading(ErrorType::PdoStateTransitionError);
        }
        break;

      case DriveState::SwitchedOn:
        switch (currentDriveState) {
          case DriveState::SwitchOnDisabled:
            controlword.setStateTransition2();
            break;
          case DriveState::ReadyToSwitchOn:
            controlword.setStateTransition3();
            break;
          case DriveState::SwitchedOn:
            MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::getNextStateTransitionControlword] "
                              << "drive state has already been reached for '"
                              << name_ << "'");
            addErrorToReading(ErrorType::PdoStateTransitionError);
            break;
          case DriveState::OperationEnabled:
            controlword.setStateTransition5();
            break;
          case DriveState::QuickStopActive:
            controlword.setStateTransition12();
            break;
          case DriveState::Fault:
            controlword.setStateTransition15();
            break;
          default:
            MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::getNextStateTransitionControlword] "
                              << "PDO state transition not implemented for '"
                              << name_ << "'");
            addErrorToReading(ErrorType::PdoStateTransitionError);
        }
        break;

      case DriveState::OperationEnabled:
        switch (currentDriveState) {
          case DriveState::SwitchOnDisabled:
            controlword.setStateTransition2();
            break;
          case DriveState::ReadyToSwitchOn:
            controlword.setStateTransition3();
            break;
          case DriveState::SwitchedOn:
            controlword.setStateTransition4();
            break;
          case DriveState::OperationEnabled:
            MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::getNextStateTransitionControlword] "
                              << "drive state has already been reached for '"
                              << name_ << "'");
            addErrorToReading(ErrorType::PdoStateTransitionError);
            break;
          case DriveState::QuickStopActive:
            controlword.setStateTransition12();
            break;
          case DriveState::Fault:
            controlword.setStateTransition15();
            break;
          default:
            MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::getNextStateTransitionControlword] "
                              << "PDO state transition not implemented for '"
                              << name_ << "'");
            addErrorToReading(ErrorType::PdoStateTransitionError);
        }
        break;

      case DriveState::QuickStopActive:
        switch (currentDriveState) {
          case DriveState::SwitchOnDisabled:
            controlword.setStateTransition2();
            break;
          case DriveState::ReadyToSwitchOn:
            controlword.setStateTransition3();
            break;
          case DriveState::SwitchedOn:
            controlword.setStateTransition4();
            break;
          case DriveState::OperationEnabled:
            controlword.setStateTransition11();
            break;
          case DriveState::QuickStopActive:
            MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::getNextStateTransitionControlword] "
                              << "drive state has already been reached for '"
                              << name_ << "'");
            addErrorToReading(ErrorType::PdoStateTransitionError);
            break;
          case DriveState::Fault:
            controlword.setStateTransition15();
            break;
          default:
            MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::getNextStateTransitionControlword] "
                              << "PDO state transition not implemented for '"
                              << name_ << "'");
            addErrorToReading(ErrorType::PdoStateTransitionError);
        }
        break;

      default:
        MELO_ERROR_STREAM("[elmo_ethercat_sdk:Elmo::getNextStateTransitionControlword] "
                          << "PDO state cannot be reached for '"
                          << name_ << "'");
        addErrorToReading(ErrorType::PdoStateTransitionError);
    }

    return controlword;
  }

  void Elmo::autoConfigurePdoSizes(){
    auto pdoSizes = bus_->getHardwarePdoSizes(static_cast<uint16_t>(address_));
    pdoInfo_.rxPdoSize_ = pdoSizes.first;
    pdoInfo_.txPdoSize_ = pdoSizes.second;
  }

  uint16_t Elmo::getTxPdoSize(){
    return pdoInfo_.txPdoSize_;
  }

  uint16_t Elmo::getRxPdoSize(){
    return pdoInfo_.rxPdoSize_;
  }

  void Elmo::engagePdoStateMachine(){
    // locking the mutex
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // elapsed time since the last new controlword
    auto microsecondsSinceChange =
        (std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - driveStateChangeTimePoint_)).count();

    // get the current state
    // since we wait until "hasRead" is true, this is guaranteed to be a newly
    // read value
    const DriveState currentDriveState = reading_.getDriveState();

    // check if the state change already vas successful:
    if (currentDriveState == targetDriveState_) {
      numberOfSuccessfulTargetStateReadings_++;
      if (numberOfSuccessfulTargetStateReadings_ >= configuration_.minNumberOfSuccessfulTargetStateReadings) {
        // disable the state machine
        conductStateChange_ = false;
        numberOfSuccessfulTargetStateReadings_ = 0;
        stateChangeSuccessful_ = true;
        return;
      }
    } else if (microsecondsSinceChange > configuration_.driveStateChangeMinTimeout) {
      // get the next controlword from the state machine
      controlword_ = getNextStateTransitionControlword(targetDriveState_, currentDriveState);
      driveStateChangeTimePoint_ = std::chrono::steady_clock::now();
    }

    // set the "hasRead" variable to false such that there will definitely be a
    // new reading when this method is called again
    hasRead_ = false;

  }

  void Elmo::addErrorToReading(const ErrorType& errorType){
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    reading_.addError(errorType);
  }
} // namespace elmo
