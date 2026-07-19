import os
from PIL import Image

def process_image(src, dest, size):
    if not os.path.exists(src):
        print(f"File not found: {src}")
        return
    try:
        with Image.open(src) as img:
            img = img.resize(size, Image.Resampling.LANCZOS)
            img = img.convert('RGBA') # Convert to RGBA first to handle transparency
            
            # Save as standard PNG. vita-pack-vpk prefers non-interlaced, 8-bit per channel
            img.save(dest, format='PNG', optimize=True)
            print(f"Created {dest} ({size[0]}x{size[1]})")
    except Exception as e:
        print(f"Error processing {src}: {e}")

if __name__ == '__main__':
    base_dir = "/Volumes/Seagate/PSVITA Develop/Dungeon-Hunter-2-vita"
    apk_res_dir = os.path.join(base_dir, "Dungeon-Hunter-2-HD-v1-0-2/res/drawable")
    livearea_dir = os.path.join(base_dir, "extras/livearea")
    
    # Check if directories exist
    if not os.path.exists(livearea_dir):
        os.makedirs(livearea_dir)

    # 1. gi_background.png -> bg0.png (840x500)
    bg_src = os.path.join(apk_res_dir, "gi_background.png")
    process_image(bg_src, os.path.join(livearea_dir, "bg0.png"), (840, 500))

    # 2. gi_background.png -> pic0.png (960x544)
    process_image(bg_src, os.path.join(livearea_dir, "pic0.png"), (960, 544))

    # 3. icon.png -> icon0.png (128x128)
    icon_src = os.path.join(apk_res_dir, "icon.png")
    # Some older apks use icon.png directly in root or res/drawable-hdpi. We'll try the one provided by user.
    process_image(icon_src, os.path.join(livearea_dir, "icon0.png"), (128, 128))

    # 4. DH2LOGO.jpeg -> startup.png (280x158)
    startup_src = os.path.join(base_dir, "DH2LOGO.jpeg")
    process_image(startup_src, os.path.join(livearea_dir, "startup.png"), (280, 158))
