import urllib.request
import json
import os

folders = ['libavcodec', 'libavformat', 'libavutil', 'libswscale', 'libswresample']
for f in folders:
    os.makedirs(f'external/include/{f}', exist_ok=True)
    api_url = f'https://api.github.com/repos/Core-2-Extreme/Video_player_for_3DS/contents/library/include/{f}'
    req = urllib.request.Request(api_url, headers={'User-Agent': '3DS-Downloader'})
    with urllib.request.urlopen(req) as resp:
        items = json.loads(resp.read().decode())
        for item in items:
            name = item['name']
            if item['type'] == 'file' and name.endswith('.h'):
                file_dest = os.path.join('external', 'include', f, name)
                if not os.path.exists(file_dest):
                    print(f'Fetching {f}/{name}...')
                    urllib.request.urlretrieve(item['download_url'], file_dest)
print('All headers downloaded successfully!')
