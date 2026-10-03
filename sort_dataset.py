"""
KrushiSense - automatic dataset sorter for Edge Impulse
Uses pretrained YOLOv8n to filter and clean images:
  human      -> person visible, no animal
  animal     -> animal visible, no person
  background -> NO person and NO animal (both rejected)
Doubtful/mixed images are sent to OUT_DIR/review/... (not deleted).
All accepted images are downgraded to ESP32-CAM like quality (320x240, soft blur, JPEG compression).
"""

import random
import io
import shutil
from pathlib import Path
from PIL import Image, ImageFilter, ImageEnhance
from ultralytics import YOLO

# ==========================================
# 1. PATHS AUR CONFIGURATION (APNE HISAAB SE EDIT KARO)
# ==========================================
# Output jahan clean dataset save hoga
OUT_DIR = r"C:\datasets\krushisense_ready"

# Har class (human, animal, background) ke liye target count
N_PER_CLASS = 300            

# Sources list: (folder_path, hint, optional_subfolders_list)
# hint = "human" | "animal" | "background" | "auto"
SOURCES = [
    # Animals-10 dataset (sirf farm-relevant categories le rahe hain)
    (r"raw-img", "animal", ["cane", "mucca", "pecora", "cavallo", "gatto", "gallina"]),
    
    # Human dataset (folder me subfolders ho to bhi auto-detect kar lega)
    (r"human detection dataset", "human", None),
    
    # Khaali khet (empty bare field)
    (r"images", "background", None),
    
    # Crops wali images (jo download_crops.py se download hongi)
    (r"bg", "background", None),
]

# ==========================================
# 2. MODEL AUR DETECTION PARAMETERS
# ==========================================
CONF = 0.35                                   # Detection threshold (0.35 is balanced)
PERSON = {0}                                  # COCO class 0: person
ANIMAL = {14, 15, 16, 17, 18, 19, 20, 21, 22, 23}   # COCO animals: bird, cat, dog, horse, sheep, cow, etc.
IMG_EXT = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}

random.seed(42)
print("Loading YOLOv8n model...")
model = YOLO("yolov8n.pt")


def collect(root):
    """Folder ke andar ke saare valid image files nikalta hai."""
    files = [p for p in Path(root).rglob("*") if p.suffix.lower() in IMG_EXT]
    random.shuffle(files)
    return files


def camera_like(img):
    """High-res web image ko low-cost ESP32-CAM jaisa blurry aur compressed banata hai."""
    img = img.convert("RGB")
    img.thumbnail((640, 480))
    img = img.resize((320, 240), Image.BILINEAR)
    
    # Halki blur simulate karna
    if random.random() < 0.7:
        img = img.filter(ImageFilter.GaussianBlur(random.uniform(0.4, 1.2)))
    
    # Light variation
    img = ImageEnhance.Brightness(img).enhance(random.uniform(0.6, 1.3))
    img = ImageEnhance.Color(img).enhance(random.uniform(0.7, 1.1))
    
    # Low quality JPEG artifacts
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=random.randint(35, 65))
    buf.seek(0)
    return Image.open(buf)


def detect(path):
    """Image me person aur animal detect karta hai."""
    res = model.predict(str(path), conf=CONF, imgsz=640, verbose=False)[0]
    cls = {int(c) for c in res.boxes.cls.tolist()}
    has_person = bool(cls & PERSON)
    has_animal = bool(cls & ANIMAL)
    return has_person, has_animal


out = Path(OUT_DIR)
counters = {"human": 0, "animal": 0, "background": 0}
stats = {}


def save(img_path, folder, name):
    d = out / folder
    d.mkdir(parents=True, exist_ok=True)
    camera_like(Image.open(img_path)).save(d / name, quality=90)


def save_review(img_path, reason, name):
    d = out / "review" / reason
    d.mkdir(parents=True, exist_ok=True)
    im = Image.open(img_path).convert("RGB")
    im.thumbnail((320, 240))
    im.save(d / name, quality=85)


# ---- Group banana aur quota divide karna ----
groups = []
for folder, hint, subs in SOURCES:
    if not Path(folder).exists():
        print("WARNING: Folder not found, skipping:", folder)
        continue
    if subs:
        found = []
        for s in subs:
            ds = [d for d in Path(folder).rglob(s) if d.is_dir()]
            if ds:
                found.append((s, collect(ds[0])))
            else:
                print(f"WARNING: Sub-folder '{s}' not found in {folder}")
        for s, files in found:
            groups.append((hint, files, None, f"{Path(folder).name}/{s}"))
    else:
        groups.append((hint, collect(folder), None, Path(folder).name))

# Har class ke source folders me barabar quota split karna
for label in ("human", "animal", "background"):
    g_idx = [i for i, g in enumerate(groups) if g[0] == label]
    for i in g_idx:
        h, files, _, name = groups[i]
        groups[i] = (h, files, max(1, N_PER_CLASS // len(g_idx)), name)


def accept(label, name_prefix, g_name, src):
    counters[label] += 1
    stats[g_name]["kept"] += 1
    save(src, label, f"{name_prefix}_{counters[label]:04d}.jpg")


# ---- Main Sorting Loop ----
for hint, files, quota, g_name in groups:
    if hint == "auto":
        quota = N_PER_CLASS
    stats[g_name] = {"kept": 0, "review": 0}
    print(f"\n[{g_name}] hint={hint} | target quota={quota} | total found={len(files)}")
    
    for n, f in enumerate(files):
        if stats[g_name]["kept"] >= quota:
            break
        try:
            has_p, has_a = detect(f)
            tag = f"{g_name.replace('/', '_')}_{f.stem}.jpg"
            
            if has_p and has_a:
                save_review(f, "mixed_person_and_animal", tag)
                stats[g_name]["review"] += 1
            elif hint == "background":
                if has_p or has_a:
                    save_review(f, "background_had_person_or_animal", tag)
                    stats[g_name]["review"] += 1
                else:
                    accept("background", "background", g_name, f)
            elif hint == "human":
                if has_p:
                    accept("human", "human", g_name, f)
                else:
                    save_review(f, "human_no_person_found", tag)
                    stats[g_name]["review"] += 1
            elif hint == "animal":
                if has_a:
                    accept("animal", "animal", g_name, f)
                else:
                    save_review(f, "animal_no_animal_found", tag)
                    stats[g_name]["review"] += 1
            elif hint == "auto":
                if has_p:
                    accept("human", "human", g_name, f)
                elif has_a:
                    accept("animal", "animal", g_name, f)
                else:
                    save_review(f, "auto_nothing_found", tag)
                    stats[g_name]["review"] += 1
        except Exception as e:
            print(f"Error skipping {f.name}: {e}")
            
        if (n + 1) % 50 == 0:
            print(f"  Processed {n+1} files... Kept so far: {stats[g_name]['kept']}")

print("\n" + "=" * 25 + " SUMMARY " + "=" * 25)
for g, s in stats.items():
    print(f"{g:35s} Kept: {s['kept']:4d} | Sent to Review: {s['review']:4d}")
print("TOTAL ACCEPTED ->", counters)
print("Output directory:", out)
print("Tip: Agar target kam pad raha ho to 'review' folder check kar ke manually move kar sakte ho.")