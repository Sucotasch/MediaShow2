import zipfile
import os

def create_wlx_archive(dll_path, archive_name, dll_internal_name, inf_content):
    """Create a .wlx/.wlx64 ZIP archive"""
    with zipfile.ZipFile(archive_name, 'w', zipfile.ZIP_DEFLATED) as zf:
        zf.write(dll_path, dll_internal_name)
        zf.writestr('pluginst.inf', inf_content)
    print(f"  Created: {os.path.basename(archive_name)} ({os.path.getsize(archive_name):,} bytes)")

def main():
    base_dir = os.path.dirname(os.path.abspath(__file__))
    build_x86 = os.path.join(base_dir, 'build', 'bin', 'Release', 'MediaShow2.dll')
    build_x64 = os.path.join(base_dir, 'build_x64', 'bin', 'Release', 'MediaShow2_x64.dll')
    output_dir = base_dir

    inf_32 = """[plugininstall]
description=MediaShow2 - multimedia lister plugin for Total Commander
type=wlx
file=MediaShow2.wlx
defaultdir=Wlx\\MediaShow2
defaultextension=AVI,MPEG,MPG,ASF,VOB,MP1,MP2,MP3,WAV,OGG,WMA,DAT,MKV,WEBM,MP4,M4A,FLAC,AAC,OPUS,MID,MIDI,KAR
"""

    inf_64 = """[plugininstall]
description=MediaShow2 x64 - multimedia lister plugin for Total Commander
type=wlx
file=MediaShow2.wlx64
defaultdir=Wlx\\MediaShow2
defaultextension=AVI,MPEG,MPG,ASF,VOB,MP1,MP2,MP3,WAV,OGG,WMA,DAT,MKV,WEBM,MP4,M4A,FLAC,AAC,OPUS,MID,MIDI,KAR
"""

    print("Creating MediaShow2 plugin archives...")

    if os.path.exists(build_x86):
        wlx_path = os.path.join(output_dir, 'MediaShow2.wlx')
        create_wlx_archive(build_x86, wlx_path, 'MediaShow2.wlx', inf_32)
    else:
        print(f"  SKIP x86: {build_x86} not found")

    if os.path.exists(build_x64):
        wlx64_path = os.path.join(output_dir, 'MediaShow2.wlx64')
        create_wlx_archive(build_x64, wlx64_path, 'MediaShow2.wlx64', inf_64)
    else:
        print(f"  SKIP x64: {build_x64} not found")

    print("\nDone! Install by double-clicking the .wlx/.wlx64 file in Total Commander.")

if __name__ == '__main__':
    main()
