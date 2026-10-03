# download_crops.py
from icrawler.builtin import BingImageCrawler
import os

queries = [
    "wheat field ground level farm",
    "cotton crop field india ground level",
    "soybean farm field rows",
    "sugarcane field ground view",
    "farm gate fence rural field"
]

base_dir = r"C:\bg\web"

for q in queries:
    folder_name = q.split()[0]  # wheat, cotton, etc.
    save_path = os.path.join(base_dir, folder_name)
    crawler = BingImageCrawler(storage={'root_dir': save_path})
    # har query se 50-60 images kaafi hain
    crawler.crawl(keyword=q, max_num=60)

print("Crops download complete!")