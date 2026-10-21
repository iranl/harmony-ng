import json
import os
import sys

def migrate_backup(backup_path, out_path):
    with open(backup_path, 'r', encoding='utf-8') as f:
        stock = json.load(f)

    acts_arr = []
    for a in stock.get('Activities', []):
        aid = a.get('Id-') or a.get('Id')
        id_str = str(aid) if aid is not None else ''
        if not id_str or id_str == '0':
            continue

        name = a.get('Name') or ('PowerOff' if id_str == '-1' else 'Activity')
        suggested = a.get('SuggestedDisplay') or a.get('Type') or ('PowerOff' if id_str == '-1' else 'Generic')
        if isinstance(suggested, int):
            suggested = 'PowerOff' if id_str == '-1' else 'Custom'
        order = a.get('ActivityOrder') or a.get('Order') or 1
        
        # deviceIds: sort roles by PowerOnOrder
        dev_ids = []
        roles = a.get('Roles') or []
        sorted_roles = sorted(roles, key=lambda r: int(r.get('PowerOnOrder') if r.get('PowerOnOrder') is not None else (r.get('Order') if r.get('Order') is not None else 999)))
        for r in sorted_roles:
            rdid = r.get('DeviceId-') or r.get('DeviceId')
            if rdid:
                srd = str(rdid)
                if srd != '0' and srd not in dev_ids:
                    dev_ids.append(srd)
                    
        def parse_seq(seq_list):
            out = []
            for s in (seq_list or []):
                stype = s.get('Type')
                st_type = s.get('__type') or ''
                tstr = stype if stype else ('Delay' if 'Delay' in st_type else 'IRCommand')
                sdid = s.get('DeviceId-') or s.get('DeviceId') or '0'
                sd_str = str(sdid)
                cmd = s.get('CommandName') or s.get('IRCommandName') or s.get('Command') or ''
                del_val = s.get('Duration') if s.get('Duration') is not None else s.get('Delay', 0)
                if tstr == 'Delay' and del_val <= 0:
                    del_val = 1000
                out.append({
                    'type': tstr,
                    'deviceId': sd_str,
                    'command': cmd,
                    'delay': int(del_val)
                })
            return out

        start_seq = parse_seq(a.get('StartSequence') or a.get('EnterActions'))
        stop_seq = parse_seq(a.get('StopSequence') or a.get('LeaveActions'))

        if not dev_ids:
            for st in start_seq + stop_seq:
                did = st.get('deviceId')
                if did and did != '0' and did not in dev_ids:
                    dev_ids.append(did)

        acts_arr.append({
            'id': id_str,
            'name': name,
            'type': suggested,
            'order': int(order),
            'icon': 'tv',
            'deviceIds': dev_ids,
            'startSequence': start_seq,
            'stopSequence': stop_seq
        })

    clean_doc = {'activities': acts_arr}
    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(clean_doc, f, indent=2)
    print(f"Successfully migrated {len(acts_arr)} activities from {backup_path} -> {out_path}")

if __name__ == '__main__':
    src = sys.argv[1] if len(sys.argv) > 1 else 'backups/activities.json'
    dst = sys.argv[2] if len(sys.argv) > 2 else 'backups/activities_migrated.json'
    migrate_backup(src, dst)
