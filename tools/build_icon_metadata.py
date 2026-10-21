import os
import glob
import re
import json
from PIL import Image

ICONS_DIR = r'data\remote_icons\icons'

# 1. Base command mappings (Logitech Function IDs)
COMMAND_MAP = {
    '1': 'Television',
    '2': 'Fast Forward (>>)',
    '4': 'Play (>)',
    '5': 'Rewind (<<)',
    '6': 'Stop ([])',
    '7': 'Green A Button (Xbox)',
    '8': 'Red B Button (Xbox)',
    '9': 'Blue X Button (Xbox)',
    '10': 'Yellow Y Button (Xbox)',
    '14': 'Play (Triangle)',
    '16': 'Cable / Satellite STB',
    '32': 'Square A Button',
    '33': 'Plus (+)',
    '36': 'Aspect Ratio / Scale',
    '37': 'Headphones / Audio',
    '38': 'Square B Button',
    '39': 'Replay / Loop Arrow',
    '40': 'Square Icon',
    '41': 'TV Guide / Open Book',
    '42': 'Brightness / Sun',
    '43': 'Settings / Cogwheel',
    '44': 'Search Document / Find',
    '49': 'Square C Button',
    '53': 'Red Circle Button (PlayStation)',
    '57': 'Blue Cross Button (PlayStation)',
    '58': 'Square D Button',
    '62': 'Chevron Down (v)',
    '63': 'Chevron Left (<)',
    '64': 'Chevron Right (>)',
    '65': 'Chevron Up (^)',
    '66': 'OK / Select Bullseye',
    '67': 'Disc 1',
    '68': 'Disc 2',
    '69': 'Disc 3',
    '70': 'Disc 4',
    '71': 'Disc 5',
    '72': 'Disc 6',
    '74': 'Eject Disc',
    '75': 'Exit / Door',
    '76': 'Fast Forward (>>)',
    '77': 'Favorite (Star)',
    '80': 'Step Forward / Slow (|>) ',
    '83': 'Green Solid Button',
    '85': 'Target / Dial',
    '88': 'Audio / Cassette',
    '89': 'Compass / Dial',
    '90': 'Menu Grid',
    '91': 'Mute / Sound Off',
    '92': 'Antenna / Live TV',
    '93': 'Teletext / Page',
    '94': 'Previous / Back Arrow',
    '96': 'Number Keypad (3x3 Grid)',
    '101': 'Refresh / Circular Arrow',
    '104': 'Page Down (vv)',
    '105': 'Page Up (^^)',
    '106': 'Pause (||)',
    '112': 'Picture in Picture (PiP)',
    '113': 'Play (Solid Triangle)',
    '114': 'Blue Solid Circle',
    '115': 'Record (Red Circle)',
    '116': 'Power Off (Standby)',
    '123': 'Numeric Keypad (123)',
    '125': 'Red Solid Circle',
    '127': 'Solid Square',
    '128': 'Return / Curve Arrow',
    '130': 'Replay / Loop Arrow',
    '131': 'Rewind (<<)',
    '135': 'Search / Magnifier',
    '141': 'Skip Backward (|<)',
    '142': 'Fullscreen / Expand',
    '143': 'Slow Rewind (<|)',
    '144': 'Slow Forward (|>) ',
    '145': 'Sleep Timer (Moon)',
    '147': 'Record / Red Dot',
    '148': 'Stop ([])',
    '149': 'Subtitles / Speech Bubble',
    '150': 'Aspect Ratio (16:9)',
    '158': 'Yellow Dot',
    '159': 'Audio Track / Music Note',
    '160': 'Live Broadcast',
    '161': 'DVR / Recorded TV',
    '162': 'TV Guide (Calendar)',
    '163': 'Info (i)',
    '166': 'Volume Down (-)',
    '171': 'Volume Up (+)',
    '172': 'Yellow Solid Circle',
    '173': 'Aspect Ratio / Wide',
    '193': 'Home House',
    '195': 'Light Bulb (Outline)',
    '196': 'Padlock (Locked)',
    '197': 'Padlock (Unlocked)',
    '200': 'Door / Exit',
    '201': 'Fan / Airflow',
    '203': 'Thermostat / Temperature',
    '204': 'Radiator / Heater',
    '207': 'Water Droplet / Moisture',
    '209': 'Flame / Heating',
    '212': 'Window Blinds',
    '215': 'Power Socket / Plug',
    '223': 'Angled Power Plug',
    '225': 'Motion Sensor',
    '226': 'Light Bulb (Off)',
    '227': 'Light Bulb (On)',
    '259': 'Timer Preset 1',
    '260': 'Timer Preset 2',
    '261': 'Timer Preset 3',
    '262': 'Timer Preset 4',
    '267': 'Timer Preset 5',
    '268': 'Timer Preset 6',
    '269': 'Timer Preset 7',
    '270': 'Timer Preset 8',
    'playpause': 'Play / Pause (⏯)',
    'numpad': 'Number Keypad',
    'sequence': 'Command Sequence / Macro',
    'color1': 'Red Button',
    'color2': 'Green Button',
    'color3': 'Yellow Button',
    'color4': 'Blue Button',
    'color5': 'Magenta Button',
}

# 2. Known Device & Named Mappings
DEVICE_NAMED_MAP = {
    'tv': 'Television / Smart TV',
    'receiver': 'AV Receiver / Amplifier',
    'generic': 'Generic Device',
    'ps': 'Sony PlayStation',
    'xbox': 'Microsoft Xbox',
    'shield': 'NVIDIA Shield TV',
    'sonos': 'Sonos Speaker',
    'heos': 'Denon HEOS Audio',
    'firetv': 'Amazon Fire TV',
    'pc': 'Computer / PC',
    'cd': 'CD Player / Disc',
    'dvd': 'DVD / Blu-ray Player',
    'switch': 'Nintendo Switch',
    'wiihome': 'Nintendo Wii Home',
    'music': 'Music / Audio',
    'watchtv': 'Watch TV',
    'slidelock': 'Slide to Unlock',
    'settings': 'Settings',
    'numpad': 'Numeric Keypad',
    'numpad_preview': 'Keypad Preview',
    'gesture_preview': 'Touch Gestures',
    'gesture_cue': 'Gesture Guide',
    'footermenu': 'Bottom Bar Menu',
    'footermenu_sleeptimer': 'Sleep Timer Menu',
    'hopsmenu': 'Activity Navigation Menu',
    'hopsmenu_sleeptimer': 'Sleep Timer',
    'flip_device': 'Switch Device View',
    'flip': 'Page Flip',
    'expand': 'Expand View',
    'collapse': 'Collapse View',
    'pull_up': 'Pull Up Sheet',
    'pull_down': 'Pull Down Sheet',
    'add': 'Add',
    'close': 'Close',
    'help': 'Help Support',
    'help2': 'Help Assistant',
    'edit': 'Edit',
    'edit_lock': 'Edit Locked',
    'low-battery': 'Low Battery Alert',
    'charging': 'Battery Charging',
    'level100': 'Battery 100%',
    'level50': 'Battery 50%',
    'level25': 'Battery 25%',
    'alert_big': 'Alert Warning',
    'alert_small': 'Alert Warning',
    'bri': 'Brightness',
    'ct': 'Color Temperature',
    'less': 'Decrease',
    'more': 'Increase',
    'minus_normal': 'Minus Button',
    'checkmark': 'Checkmark',
    'tick': 'Tick Mark',
    'checkbox': 'Checkbox (Empty)',
    'checkbox_true': 'Checkbox (Checked)',
    'scene_icon': 'Smart Scene',
    'doorlock_unlock': 'Door Lock (Unlocked)',
    'doorlock_lock': 'Door Lock (Locked)',
    'nest_away': 'Nest Thermostat (Away)',
    'nest_home': 'Nest Thermostat (Home)',
    'presence_on': 'Presence Sensor (Present)',
    'presence_off': 'Presence Sensor (Away)',
    'sensor_on': 'Motion Sensor (Triggered)',
    'sensor_off': 'Motion Sensor (Idle)',
    'blinds_on': 'Window Blinds (Open)',
    'blinds_off': 'Window Blinds (Closed)',
    'plug_on': 'Smart Plug (On)',
    'plug_off': 'Smart Plug (Off)',
    'bulb': 'Light Bulb',
    'bulb_on': 'Light Bulb (On)',
    'bulb_off': 'Light Bulb (Off)',
    'bulb_small': 'Light Bulb (Small)',
    'thermostat_on': 'Thermostat (Active)',
    'thermostat_off': 'Thermostat (Idle)',
    'waterheater_on': 'Water Heater (On)',
    'waterheater_off': 'Water Heater (Off)',
    'poweroff': 'Power Off All',
}

# 3. Known UUID base mappings based on verified visual inspection & Logitech schemas
UUID_MAP = {
    '013A19AA-DFB1-40E8-80A3-773C7A0AE986': ('TV with Antenna / Broadcast', 'activity', ['tv', 'antenna', 'broadcast']),
    '01D34356-613C-45F3-935D-4BF763AE4F4B': ('Smart Desk Lamp', 'home', ['lamp', 'light', 'desk']),
    '053FA80A-458F-4EA9-85E5-B2F7DB1C7052': ('Subwoofer / Bass Sound', 'activity', ['subwoofer', 'audio', 'speaker']),
    '085A8031-090D-4052-BB5F-C7809DCE8665': ('Curved Display / Gaming Monitor', 'activity', ['monitor', 'screen', 'curved']),
    '0D75B8C1-3EE5-40C2-995E-34C8ADEDAAA4': ('Set-Top Box with Clock', 'activity', ['stb', 'cable', 'clock']),
    '0DF1488A-EEDB-40EC-8BC7-B87E16E13DC2': ('Electric Guitar / Rock Music', 'activity', ['guitar', 'music', 'rock']),
    '0E588723-18DA-4DF7-978F-0306F601FE28': ('Ceiling Fan with Light', 'home', ['fan', 'ceiling', 'cooling']),
    '0F40F257-68C8-4F24-9561-3CB3441DA395': ('Apple TV', 'activity', ['apple', 'appletv', 'stream']),
    '1861D98D-B831-4DD7-96DD-58FD0EA1B045': ('Video Game Controller', 'activity', ['gamepad', 'console', 'play']),
    '1978F30F-E27F-4126-B622-FC230936AE2F': ('Blu-ray / DVD Disc Tray', 'activity', ['disc', 'bluray', 'dvd']),
    '204F0071-4B64-4577-BDA9-E726D6480ABF': ('Media Player Box', 'activity', ['player', 'media', 'box']),
    '21AA7D88-E238-49B3-9933-276232B7DC6C': ('Retro Gamepad / D-Pad', 'activity', ['retro', 'controller', 'game']),
    '257C953F-B575-4F4D-8429-F8EAA1DE8241': ('Floor Lamp / Reading Light', 'home', ['lamp', 'floor', 'reading']),
    '27BBF517-B5EA-46CE-8155-08F6D48E0BCE': ('Over-Ear Headphones', 'activity', ['headphones', 'audio', 'listen']),
    '29281B8D-FC8D-4C13-89C4-60705B8429F9': ('AV Receiver with Volume Knob', 'activity', ['avr', 'receiver', 'sound']),
    '31179E2F-BD17-48E9-95C0-22CEB36C6188': ('Home Theater Projector', 'activity', ['projector', 'cinema', 'theater']),
    '311D40B5-6CF9-4DA2-8820-E17012B852A1': ('Stereo Bookshelf Speakers', 'activity', ['speakers', 'stereo', 'bookshelf']),
    '349826B4-FA41-4AF4-B032-D5280B1363AC': ('Smart Speaker Waves', 'home', ['speaker', 'audio', 'smart']),
    '363CE48C-B589-48FA-99D9-6AE5056C1E9C': ('Compact Disc / CD Audio', 'activity', ['cd', 'disc', 'audio']),
    '36D512D1-9F0B-4084-89A1-63B6947E9587': ('Studio Headphones', 'activity', ['headphones', 'studio', 'music']),
    '3AEBFA09-DE9A-46F5-86FB-AC5E585295FB': ('Handheld Gaming Console', 'activity', ['console', 'portable', 'handheld']),
    '3BF99E7B-9904-4AFA-8D3D-7337CD8E9BFD': ('Watch TV / Television', 'activity', ['tv', 'television', 'watch']),
    '3D057F31-87AD-4AD5-BCFD-453403A21D29': ('Smart Home Hub / Mesh Wi-Fi', 'home', ['hub', 'wifi', 'network']),
    '3D423017-BA9E-4132-A885-69222CFE4842': ('Cinema Popcorn / Movie Night', 'activity', ['popcorn', 'movie', 'cinema']),
    '3FB5EEAD-419D-472F-8232-3BBBED1F4874': ('Karaoke Microphone', 'activity', ['mic', 'microphone', 'karaoke']),
    '41BF3AA6-DA1D-4115-A225-E99F475D7DC7': ('Wall Sconce / Ambient Light', 'home', ['light', 'sconce', 'ambient']),
    '46700828-34B6-42B9-9D6A-968BC681172A': ('Soundbar Speaker', 'activity', ['soundbar', 'audio', 'tv']),
    '468668AB-2587-4662-9E89-CF66BD70F314': ('PC Desktop / Computer', 'activity', ['pc', 'computer', 'desktop']),
    '4A703D51-717D-4BD3-8457-3442879A7DCC': ('Satellite Dish / DVB', 'activity', ['satellite', 'dish', 'broadcast']),
    '4AE30392-221F-4718-9E17-ACE55F27CD3D': ('Smart Ceiling Light', 'home', ['ceiling', 'light', 'lamp']),
    '4D002443-A954-47C3-98C5-F24E53B1E628': ('Streaming Media Stick', 'activity', ['stick', 'streaming', 'hdmi']),
    '50EA2DE1-D490-4C34-BDA9-F93B5B4342FC': ('Digital SLR Camera', 'activity', ['camera', 'photo', 'dslr']),
    '5180221E-8F0C-459E-A61C-6D6E02BB2A0C': ('Floor Standing Speaker', 'activity', ['tower', 'speaker', 'hifi']),
    '5248477C-D901-49E0-A8A6-24F212443817': ('Smart Doorbell / Camera', 'home', ['doorbell', 'camera', 'entry']),
    '53469038-7E1C-4241-B9BD-BAE95CC0509C': ('Custom Activity / Star', 'activity', ['star', 'custom', 'favorite']),
    '56887E6F-3C8C-4FB5-B25F-CA027E8CBC3C': ('Table Lamp with Shade', 'home', ['lamp', 'shade', 'table']),
    '58DF2164-056E-4965-A911-CAE9621E0885': ('Smart Thermostat Dial', 'home', ['thermostat', 'dial', 'climate']),
    '5BBC2BC3-551A-493B-B374-3EC77EB5B1EB': ('Smart Socket Outlet', 'home', ['socket', 'outlet', 'plug']),
    '5BE0F80A-D144-40B0-A467-B67A7633282A': ('Surround Sound 5.1 System', 'activity', ['surround', '5.1', 'theater']),
    '5F5E0C9D-1D5A-4F6F-9DD4-243AFCCD8285': ('Amazon Fire TV', 'activity', ['firetv', 'amazon', 'streaming']),
    '6231922F-AB2E-4AE1-A111-41B09B6F3E08': ('Smart Window Shades', 'home', ['shades', 'blinds', 'window']),
    '63677E44-C3AC-46CB-873D-E948BB64A727': ('Pendant Hanging Light', 'home', ['pendant', 'hanging', 'light']),
    '64657363-41A2-4EBD-A5FD-BA7CF83DE4D3': ('Streaming Box / IPTV', 'activity', ['iptv', 'streaming', 'box']),
    '68AA6991-A06F-49FA-9BE6-FE882E03D979': ('Smart Door Lock', 'home', ['lock', 'door', 'security']),
    '6D621A24-72D6-4CCD-8ED8-1114B52952AE': ('Multiroom Audio Group', 'activity', ['multiroom', 'audio', 'group']),
    '6E24F18E-EA94-4A87-A4AE-1F2FAF1A1099': ('Internet Radio / Podcast', 'activity', ['radio', 'podcast', 'stream']),
    '6EBD03B8-C815-48BE-ADBB-D36E06CAAC5E': ('Roku Streaming Player', 'activity', ['roku', 'player', 'streaming']),
    '718B691B-DF39-4787-989D-EAB9D39D595E': ('Cable TV Box / DVR', 'activity', ['cable', 'dvr', 'tv']),
    '72D0AAB4-2ED8-4644-84D2-0031BDAE356E': ('Listen to Music / Audio', 'activity', ['music', 'audio', 'listen']),
    '7A38712D-0BB0-4D3C-B9AF-D54FF86CC9CC': ('Turntable / Vinyl Record', 'activity', ['vinyl', 'turntable', 'record']),
    '7B7FF0C6-CC71-498D-8B1E-C029CBB07CF3': ('Air Conditioner / AC Unit', 'home', ['ac', 'cooling', 'climate']),
    '818AD711-EAC5-42E8-80DC-D5ED9F1046F4': ('Smart Power Strip', 'home', ['strip', 'power', 'plugs']),
    '818D06A7-7821-4380-8A62-719A9D6B9C9B': ('Spotlight / Track Lighting', 'home', ['spotlight', 'track', 'light']),
    '819ABAB3-94AD-4798-8B71-BD98C829EB89': ('FM / DAB Tuner Radio', 'activity', ['radio', 'tuner', 'fm']),
    '835B9806-A8F5-4395-9AE7-A1086D75F5D3': ('Smart Roller Shutters', 'home', ['shutters', 'roller', 'blinds']),
    '840AD7C6-5295-465F-A6AE-9BC9154B6549': ('Smart Thermostat Flame (Heating)', 'home', ['heating', 'flame', 'heat']),
    '86D46E83-04EA-460A-B3DC-70A0246C7794': ('Smart Dimmer Slider', 'home', ['dimmer', 'slider', 'light']),
    '897101B2-567B-11E7-907B-A6006AD3DBA0': ('Smart Scene Mode', 'home', ['scene', 'routine', 'smart']),
    '8A4CC6CD-16E8-40E4-8C91-2917076F967D': ('Smart LED Light Strip', 'home', ['strip', 'led', 'light']),
    '8B271ADA-D2ED-477B-BFF2-D2450245C3B0': ('Audio DAC / Pre-Amp', 'activity', ['dac', 'preamp', 'hifi']),
    '8C07FD25-A715-4A64-A4F9-46B0B7A0C1EF': ('Chandelier / Luxury Light', 'home', ['chandelier', 'luxury', 'light']),
    '93CE3ABA-5FED-4374-ADC1-D5CD36DEDB36': ('Smart Radiator Valve', 'home', ['radiator', 'valve', 'heat']),
    '9532487A-4FBF-4BEF-ADD3-3CDE6EE8DB16': ('Smart Color Bulb (RGB)', 'home', ['rgb', 'color', 'bulb']),
    '970C1E6E-9E37-48F1-BAF0-EABDDB9E989B': ('Smart Security Alarm', 'home', ['alarm', 'security', 'sensor']),
    '974D2E15-D103-463A-8386-36B9EBD4037F': ('Smart Scene / Moon & Stars', 'home', ['night', 'sleep', 'scene']),
    '9B0478B4-CB51-4CC8-B58E-682033DFC8EF': ('LaserDisc / Optical Media', 'activity', ['laserdisc', 'disc', 'player']),
    '9C0B36C2-F503-41FA-B957-4A0BCC7E4D8F': ('Smart Light Switch', 'home', ['switch', 'wall', 'light']),
    '9CCF9A96-B984-4AEB-9227-9B565FF74E06': ('Karaoke Machine / Jukebox', 'activity', ['karaoke', 'jukebox', 'sing']),
    '9CEC604E-B876-4953-88D2-7CD6283D3275': ('Smart Garage Door Opener', 'home', ['garage', 'door', 'motor']),
    '9D24EB35-468B-4148-A6A2-BB764AA9609C': ('Play Game / Gamepad', 'activity', ['game', 'gamepad', 'console']),
    '9D3EA5D0-9E27-4488-922E-F706C7FA1A8C': ('Music Cassette Tape', 'activity', ['tape', 'cassette', 'retro']),
    '9D6B957C-8A14-4C92-ADEE-DD0C8FBB2366': ('Smart Garden Sprinkler', 'home', ['sprinkler', 'garden', 'irrigation']),
    'A1164C5F-1D7A-4B7F-BBEC-E9F5ACF303B7': ('Over-The-Air TV Antenna', 'activity', ['antenna', 'ota', 'tuner']),
    'A12FF2BE-5C15-4E5D-8E51-1F328716EF3D': ('Audio Equalizer (EQ)', 'activity', ['eq', 'equalizer', 'audio']),
    'A181D535-D966-4178-91F9-57278BE8D7EA': ('Smart Outdoor Floodlight', 'home', ['floodlight', 'outdoor', 'light']),
    'A3A7FE08-E4DD-4B05-B0A9-945CD717A09B': ('Smart Air Purifier', 'home', ['purifier', 'air', 'clean']),
    'A54AB6F8-8FBE-4A90-A43E-2C9993961EFD': ('Soundboard / DJ Mixer', 'activity', ['dj', 'mixer', 'soundboard']),
    'A648AC27-8B04-4CAD-A2B2-82A83BCDB741': ('Smart Humidifier', 'home', ['humidifier', 'moisture', 'climate']),
    'AB95C212-7B27-4118-8CD3-0E595EA061D5': ('Streaming Audio Player', 'activity', ['streamer', 'audio', 'player']),
    'AC921110-8F9C-40A3-B51C-06F3B1E18C92': ('Smart Security Camera', 'home', ['cctv', 'camera', 'security']),
    'B25A8282-30CE-4EA3-9F92-3E25474B076B': ('Home Cinema Projector Screen', 'activity', ['screen', 'cinema', 'projector']),
    'B4241984-3855-46C7-A940-3604743297BD': ('Smart Water Heater Boiler', 'home', ['boiler', 'water', 'heater']),
    'BAB09C4A-E61D-4284-B1F3-84135F562217': ('Watch Movie / Film', 'activity', ['movie', 'film', 'cinema']),
    'BF4C747E-C437-423E-9801-535E0EA55848': ('Smart Vacuum Cleaner / Robot', 'home', ['vacuum', 'robot', 'cleaning']),
    'C45B3641-9CE3-43D8-907F-F87838A86C84': ('Multi-Disc Carousel Player', 'activity', ['carousel', 'multidisc', 'player']),
    'C4D124B8-F851-4200-86C0-B38494FE6167': ('Wireless Subwoofer', 'activity', ['subwoofer', 'wireless', 'bass']),
    'C62C7998-D6BF-4C2D-9E6C-47440E28A113': ('Smart Home Door Sensor', 'home', ['sensor', 'door', 'contact']),
    'C83A52E3-5615-4A0B-987E-58C3D85FFCCD': ('Smart Window Open Sensor', 'home', ['sensor', 'window', 'contact']),
    'CB589261-82DB-4113-BC1B-C09EC6E17B2C': ('Smart Outdoor Weather Station', 'home', ['weather', 'temperature', 'outdoor']),
    'CD78E011-E7FD-42EA-B6D8-FAFCA64B9C09': ('Smart Siren / Alarm Bell', 'home', ['siren', 'bell', 'security']),
    'CEDA3F57-1D34-48F0-852D-EC9C425B2733': ('Smart Water Leak Detector', 'home', ['leak', 'water', 'flood']),
    'CEDD3F0F-932A-4135-8F2D-1670550F20EE': ('Audio Cassette Deck', 'activity', ['cassette', 'deck', 'tape']),
    'CFA819F4-DB13-4F04-985D-D59087108D62': ('Smart Keypad Door Entry', 'home', ['keypad', 'entry', 'door']),
    'D1D5E928-D0AD-488A-A43E-56FD6D17209B': ('Smart Smoke Detector', 'home', ['smoke', 'fire', 'alarm']),
    'D22A587C-E45C-4B4A-9396-19396EB8E32B': ('Wireless Headphone Stand', 'activity', ['headphones', 'stand', 'charging']),
    'D4F91D44-F634-41FB-A9BE-FC8F63ABCED2': ('Smart Window Curtains', 'home', ['curtains', 'drapes', 'window']),
    'D636FD4E-016C-447F-BA4A-25E8943E23F4': ('Smart Gas Leak Detector', 'home', ['gas', 'detector', 'safety']),
    'D70209CE-A536-4194-84D5-1648087BB060': ('Smart Thermostat Snowflake (Cooling)', 'home', ['cooling', 'snowflake', 'ac']),
    'D7F21109-8F71-4EDB-990A-BCAB435EFB92': ('Smart Motion Floodlight', 'home', ['floodlight', 'motion', 'light']),
    'DEC07396-B858-47E8-AE50-4AB346AF1D98': ('Sony PlayStation Console', 'activity', ['playstation', 'ps', 'sony', 'game']),
    'DFF3A4B7-B2A5-41F6-8347-77CAC20A7DD0': ('Smart Pool Heater / Pump', 'home', ['pool', 'pump', 'water']),
    'E816D949-6F22-4FA2-A278-17FABE2CFCC9': ('Smart Window Sensor', 'home', ['window', 'sensor', 'security']),
    'E942EDFA-826D-4B06-9E8F-073F8895E69C': ('Smart Irrigation Valve', 'home', ['valve', 'irrigation', 'water']),
    'EAC61C29-ABB4-4545-9F69-85010B4CCAD5': ('Smart Light Bulb Globe', 'home', ['globe', 'bulb', 'light']),
    'EB6113E5-431D-4642-9348-F1FD23207B69': ('Smart Thermostat Schedule', 'home', ['schedule', 'timer', 'heat']),
    'EC6648B8-49EE-4457-95E4-F31B242D989A': ('Smart Wall Switch Dual', 'home', ['switch', 'dual', 'wall']),
    'EDCD349E-0BB5-4320-A1AE-D409BCFBA325': ('Smart Garage Sensor', 'home', ['garage', 'sensor', 'tilt']),
    'EFD06B84-DCE8-4BF7-AF0C-AFAC990862B5': ('Smart Awning Motor', 'home', ['awning', 'shade', 'motor']),
    'F30D50E9-96A2-4A44-92D5-9A42431F83DA': ('Smart Window Motor', 'home', ['window', 'motor', 'shutter']),
    'FA224AD1-A6B0-4FB3-84A1-9EE204C0DCD9': ('Smart Scent Diffuser', 'home', ['diffuser', 'scent', 'aroma']),
    'FAD05710-2621-4042-9DD4-B3466665A584': ('Smart Air Quality Monitor', 'home', ['air', 'co2', 'sensor']),
    'FCF344CE-4029-406C-8E11-582D7A7BFB08': ('Smart Light Bulb Filament', 'home', ['filament', 'bulb', 'vintage']),
    'FDC89D40-DBBF-469B-9FC9-B24A6651FE6A': ('Smart Temperature Sensor', 'home', ['temp', 'temperature', 'thermometer']),
}

def analyze_filename(fn):
    clean = fn.replace('.png', '').strip()
    
    # 1. Check direct named
    lower = clean.lower()
    if lower in DEVICE_NAMED_MAP:
        name = DEVICE_NAMED_MAP[lower]
        cat = 'device' if any(x in lower for x in ['tv', 'receiver', 'ps', 'xbox', 'shield', 'sonos', 'heos', 'firetv', 'pc', 'cd', 'dvd', 'switch', 'generic']) else ('home' if any(x in lower for x in ['bulb', 'plug', 'blind', 'thermostat', 'sensor', 'presence', 'nest', 'scene', 'doorlock', 'waterheater']) else 'ui')
        return name, cat, [clean.lower()]

    # 2. Check numeric command
    if clean.isdigit():
        name = COMMAND_MAP.get(clean, f'Button {clean}')
        return name, 'commands', ['command', 'button', clean]

    # 3. Check UUID with suffixes
    m = re.match(r'^([0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})(.*)$', clean)
    if m:
        base_uuid = m.group(1).upper()
        suffix = m.group(2).lower()
        if base_uuid in UUID_MAP:
            base_name, cat, tags = UUID_MAP[base_uuid]
            if suffix == '_small':
                name = f'{base_name}'
            elif suffix == '_on':
                name = f'{base_name} (Active)'
            elif suffix == '_smallon':
                name = f'{base_name} (Active)'
            elif suffix == '_off':
                name = f'{base_name} (Inactive)'
            else:
                name = f'{base_name} (Large)'
            return name, cat, tags + [base_uuid.lower()]
        else:
            return f'Activity {base_uuid[:8]}', 'activity', ['activity', base_uuid.lower()]

    # 4. Check device ID numbers (e.g. 1.png, 5.png, 10.png, 16.png, 33.png, 63.png)
    dev_num_map = {
        '1': 'Television (TV)',
        '5': 'AV Receiver / Amplifier',
        '10': 'DVD / Blu-ray Player',
        '16': 'Media Streamer / Box',
        '33': 'Cable / Satellite Box',
        '63': 'Video Game Console',
    }
    if clean in dev_num_map:
        return dev_num_map[clean], 'device', ['device', clean]

    # 5. Fallback clean formatting
    words = clean.replace('-', ' ').replace('_', ' ').split()
    title = ' '.join(w.capitalize() for w in words)
    return title, 'other', [clean.lower()]

# Process all files in ICONS_DIR
all_pngs = sorted([f for f in os.listdir(ICONS_DIR) if f.endswith('.png')])
metadata = {}

for fn in all_pngs:
    name, cat, tags = analyze_filename(fn)
    metadata[fn] = {
        "name": name,
        "category": cat,
        "tags": list(set(tags))
    }

output_path = r'android-app\src\icon-metadata.json'
with open(output_path, 'w', encoding='utf-8') as f:
    json.dump(metadata, f, indent=2, ensure_ascii=False)

print(f"Generated {len(metadata)} icon mappings in {output_path}!")

# Sample 15 entries
samples = list(metadata.items())[:15]
for fn, meta in samples:
    print(f"  {fn} -> {meta['name']} [{meta['category']}]")
