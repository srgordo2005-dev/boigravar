import os
import re

AUDIO_DIR = 'audio'
HEADER_FILE = 'esp32_boi_touch_interno/audios_progmem.h'

def sanitize_name(name):
    name = re.sub(r'[^a-zA-Z0-9]', '_', name)
    # Ensure it starts with a letter
    if not name[0].isalpha():
        name = 'a_' + name
    return name

def main():
    if not os.path.exists(AUDIO_DIR):
        print(f"Directory {AUDIO_DIR} not found.")
        return

    files = [f for f in os.listdir(AUDIO_DIR) if f.lower().endswith('.mp3')]
    
    with open(HEADER_FILE, 'w') as out:
        out.write('#ifndef AUDIOS_PROGMEM_H\n')
        out.write('#define AUDIOS_PROGMEM_H\n\n')
        out.write('#include <pgmspace.h>\n\n')
        
        array_names = []
        array_sizes = []
        original_names = []
        
        for f in files:
            filepath = os.path.join(AUDIO_DIR, f)
            array_name = sanitize_name(f.replace('.mp3', ''))
            with open(filepath, 'rb') as mp3:
                data = mp3.read()
            
            out.write(f'const unsigned char {array_name}[] PROGMEM = {{\n')
            
            # Format as hex
            hex_data = [f"0x{b:02X}" for b in data]
            for i in range(0, len(hex_data), 16):
                out.write('  ' + ', '.join(hex_data[i:i+16]) + ',\n')
                
            out.write('};\n\n')
            
            array_names.append(array_name)
            array_sizes.append(len(data))
            original_names.append(f)
            
        # Create an array of pointers
        out.write(f'const int num_progmem_audios = {len(array_names)};\n\n')
        
        if len(array_names) > 0:
            out.write('const unsigned char* const progmem_audios[] PROGMEM = {\n')
            for name in array_names:
                out.write(f'  {name},\n')
            out.write('};\n\n')
            
            out.write('const unsigned int progmem_audio_sizes[] = {\n')
            for size in array_sizes:
                out.write(f'  {size},\n')
            out.write('};\n\n')
            
            out.write('const char* const progmem_audio_names[] = {\n')
            for name in original_names:
                out.write(f'  "{name}",\n')
            out.write('};\n\n')
            
        out.write('#endif\n')

    print(f"Generated {HEADER_FILE} with {len(files)} audios.")

if __name__ == '__main__':
    main()
