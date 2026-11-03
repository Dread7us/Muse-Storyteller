"""Build only; this command never flashes or contacts a device."""
import json
import math
import os
from pathlib import Path
import subprocess
import sys
from urllib.parse import urlencode
from patch_lvgl import apply

ROOT=Path(__file__).resolve().parents[1]

def main():
    idf=os.environ.get('IDF_PATH')
    if not idf: raise SystemExit('Activate ESP-IDF v6.0.1 first.')
    sdk=ROOT/'.local/sdk/esp32'; build=ROOT/'.local/build'
    if not (sdk.parent/'.storyteller-overlay.json').exists(): raise SystemExit('Run tools/prepare_firmware.py first.')
    weather=ROOT/'private/weather.json'
    private_header=sdk/'components/muse/storyteller_weather_private.h'
    if weather.exists():
        config=json.loads(weather.read_text())
        lat,lon=config['latitude'],config['longitude']
        if type(lat) not in (int,float) or type(lon) not in (int,float) or not math.isfinite(lat) or not math.isfinite(lon) or not -90<=lat<=90 or not -180<=lon<=180: raise SystemExit('Invalid private weather coordinates.')
        label=config.get('label','Local weather')
        if not isinstance(label,str) or not 1<=len(label)<=40 or any(ord(c)<32 or ord(c)>126 for c in label): raise SystemExit('Use a short ASCII weather label.')
        query=dict(latitude=lat,longitude=lon,current='temperature_2m,apparent_temperature,weather_code,wind_speed_10m',daily='weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max',temperature_unit='fahrenheit',wind_speed_unit='mph',precipitation_unit='inch',timezone='auto',forecast_days=5)
        private_header.write_text('#pragma once\n#define STORYTELLER_WEATHER_URL '+json.dumps('https://api.open-meteo.com/v1/forecast?'+urlencode(query))+'\n#define STORYTELLER_WEATHER_LABEL '+json.dumps(label)+'\n')
    elif private_header.exists():
        raise SystemExit('Generated weather configuration exists but private/weather.json is missing. Review it before building.')
    command=[sys.executable,str(Path(idf)/'tools/idf.py'),'-C',str(sdk),'-B',str(build),'-DIDF_TARGET=esp32','-DSDKCONFIG='+str(build/'sdkconfig'),'-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-core2-v11']
    secrets=ROOT/'private/sdkconfig.secrets'
    if secrets.exists(): command[-1]+=';'+str(secrets).replace('\\','/')
    subprocess.run(command+['reconfigure'],check=True)
    apply()
    subprocess.run(command+['build'],check=True)
    print('Built .local/build/muse-gadget.bin. Pairing data and connected hardware were not touched.')

if __name__=='__main__': main()
