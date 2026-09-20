# Sleep and busywait silly samples

## Sleep
west build -b nucleo_f756zg -p always -d build_sleep ./samples/silly/audio/silly_sleep/

## Busy wait
west build -b nucleo_f756zg -p always -d build_busywait ./samples/silly/audio/silly_busywait/
west flash -d build_busywait/

