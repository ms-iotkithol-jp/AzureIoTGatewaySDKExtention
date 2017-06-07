# Extention 
## iothub module 
- Receive 'Device Twin Desired Property Update' notification and send json as message to broker bus. 
- Receive 'Device Twin Method' invocation adn send method_name and payload as message to broker bus.  

This capability can be used only when you specify MQTT protocol. 

## identitymap module 
- Notify registerd mapping to iothub module specified by JSON configuration argument via broker bus. 
- The iothub module receive this message then create each personality and set callbacks. 

## ble module 
- Support sequential operation for BLE sensor at initial time. 
When you want to cofirm following steps... 
1. Enable calibration mode 
2. Read calibration values 
3. Enable sensor 

You can specify this sequence as follows 
```json
  {
    "type":  "sequential",
    "instructions": [
      {
        "type": "write_at_init",
        "characteristic_uuid": "F000AA42-0451-4000-B000-000000000000",
        "data": "Ag==",
        "description" : "enable presssure sensor calibration"
      },
      {
        "type": "read_once",
        "characteristic_uuid": "F000AA43-0451-4000-B000-000000000000",
        "description":"read pressure sensor calibration config"
      },
      {
        "type": "write_at_init",
        "characteristic_uuid": "F000AA42-0451-4000-B000-000000000000",
        "data": "AQ==",
        "description":"enable pressure sensor"
      }
    ],
  }
```

## Resolver 
This modules convert raw data come from BLE sensor to physical value and produce JSON formated string. 
