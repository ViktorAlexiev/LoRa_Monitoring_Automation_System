// Plain-language titles for the error codes the backend raises. The raw
// code is never shown to the farmer.
export const ERROR_TITLES = {
  SOIL_TOO_DRY: "Почвата е твърде суха",
  SOIL_TOO_WET: "Почвата е твърде мокра",
  SENSOR_OFFLINE: "Сензор не праща данни",
  SENSOR_FAULT_255: "Сензор не може да измери",
  SENSOR_STUCK_VALUE: "Сензор показва една и съща стойност",
  SENSOR_OUT_OF_RANGE: "Сензор показва невъзможна стойност",
  SENSOR_OUTLIER: "Сензор показва различно от другите",
  VALVE_COMMAND_TIMEOUT: "Клапан не изпълни командата",
  PUMP_COMMAND_TIMEOUT: "Помпа не изпълни командата",
  VALVE_NO_EFFECT: "Клапан е отворен, но няма ефект",
  EXECUTOR_OFFLINE: "Няма връзка с модул за управление",
  LORA_REPEATER_DOWN: "Няма връзка с усилвател на сигнала",
  GATEWAY_OFFLINE: "Няма връзка с централния модул",
  PUMP_CAPACITY_EXCEEDED: "Помпата е претоварена",
  PUMP_QUEUE_WAIT: "Поливането чака ред",
  THRESHOLD_MISCONFIGURED: "Неправилни граници в настройките",
  MODULE_UNREACHABLE: "Няма връзка с модул",
};
export const errorTitle = (code) => ERROR_TITLES[code] || "Проблем";
