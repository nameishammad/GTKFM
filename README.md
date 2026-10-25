# GTK File Manager (C)

Ek simple GTK3 file manager jo:
- **ffmpeg** se video, audio (embedded album art) aur images ke thumbnails banata hai
- PDF ke liye **pdftoppm** (poppler-utils) use karta hai — kyunke ffmpeg khud PDF decode nahi kar sakta, ye is tarah ke tools ki general limitation hai
- Thumbnails ko `~/.cache/gtkfm/thumbnails/` mein cache karta hai taake baar baar generate na karne parhein
- **Sidebar** mein Places (Home, Computer, Filesystem, Desktop, Documents, Downloads, Music, Movies, Public, Trash), **Bookmarks** (Ctrl+D se current folder add karein), aur **Devices** (USB drives, disks, phones via MTP/gvfs) — mount/unmount right-click se
- **Computer** location (sidebar mein "Computer" pe click, ya path bar mein `computer://`): Windows ke "This PC" jaisa view jisme Filesystem (sabse upar) aur har mounted/unmounted device dikhta hai — unmounted device pe double-click karne se wo mount ho jata hai
- Copy / Cut / Paste: **Ctrl+C**, **Ctrl+X**, **Ctrl+V**, ya right-click menu se — files aur folders (poori tarah, recursively) dono ke liye kaam karta hai
- Right-click menu: **Open**, **Open With** (system ki registered apps se), **Open Terminal Here**, Rename, Delete (Trash), Copy, Cut, aur empty area pe New Folder / Paste
- **Drag & Drop (copy)**: file-view se select ki hui files/folders ko drag karke drop karein:
  - **Sidebar** ke kisi Place/Bookmark/Device (mounted) par → wahan copy ho jaati hain
  - **F3 split view** ke doosre pane mein (ya usi pane ke kisi folder icon par) → wahan copy ho jaati hain
  - Drag hamesha **copy** karta hai (move/delete nahi) — galat jagah drop hone se kuch delete nahi hota
- **Search bar (toolbar mein)**: kuch bhi type karein to sirf current folder nahi, balke us ke andar tamam sub-folders (aur un ke sub-folders...) mein bhi naam match karke dikhata hai. Bara directory tree ho to background thread mein search hoti hai (UI freeze nahi hoti), aur zyada se zyada 3000 matches dikhaye jaate hain.
- **Type-ahead quick search (Thunar jaisa)**: file list (icon view) par focus ho aur search bar use kiye baghair seedha koi letter type karein, to niche-dayen corner mein ek chhota box nazar aata hai jis mein typed text dikhta hai, us naam se match karti pehli file khud-ba-khud select/focus ho jaati hai. Escape se box band ho jata hai, BackSpace se aakhri letter mit jata hai, aur ~3 second khamoshi ke baad box khud gayab ho jata hai.
- **Information panel (Dolphin jaisa, F11)**: right side par ek panel show/hide hota hai jo current selection (naam, type, size, modified date, location + badi preview) dikhata hai; kuch select na ho to current folder ki info dikhati hai, aur multiple items select karne par summary (item count, total size) dikhata hai. F3 split view ke har pane ka apna independent info panel hota hai.
- **Information panel (Dolphin jaisa, F11 se toggle)**: window ke right side ek panel khulta hai jo selected file/folder ki badi preview (thumbnail/icon), naam, type, size, modified date aur location dikhata hai. Kuch select na ho to current folder ki info dikhati hai, aur ek se zyada select ho to summary (total items + size). F3 split view ke har pane ka apna independent Information panel hota hai.

## Dependencies install karein (Debian/Ubuntu)

```bash
sudo apt update
sudo apt install build-essential pkg-config libgtk-3-dev ffmpeg poppler-utils gvfs gvfs-backends
```

- `libgtk-3-dev` — GTK3 headers/libs (compile ke liye zaroori)
- `ffmpeg` — video/audio/image thumbnails
- `poppler-utils` — `pdftoppm` (PDF ka pehla page thumbnail ke liye)
- `gvfs` + `gvfs-backends` — phones ko MTP ke zariye mount karne ke liye (bina is ke phone sidebar mein nazar to aayega lekin mount nahi hoga)
- Ek terminal emulator (`gnome-terminal`, `xterm`, `konsole`, waghera koi ek) — "Open Terminal Here" ke liye

Fedora par: `sudo dnf install gtk3-devel ffmpeg poppler-utils gvfs gvfs-mtp`
Arch par: `sudo pacman -S gtk3 ffmpeg poppler gvfs gvfs-mtp`

## Build

```bash
cd gtkfm
make
```

## Run

```bash
./gtkfm
# ya
make run
```

## File structure

```
app.h         - shared struct/types
main.c        - window, toolbar, menubar
sidebar.c/.h  - Places + Devices (GVolumeMonitor) sidebar
fileview.c/.h - icon-view, navigation, context menu (Open/Open With/Terminal/Rename/Delete)
thumbnail.c/.h- ffmpeg + pdftoppm se background thumbnail generation, disk cache
infopanel.c/.h- right-side "Information" panel (Dolphin jaisa, F11 se toggle)
infopanel.c/.h- Dolphin jaisa right-side "Information" panel (F11 se toggle)
```

## Notes / limitations

- Phone ko pehli baar mount karne ke liye us par "Allow file transfer" / "MTP mode" enable karna hoga, aur `gvfs-mtp` package installed hona chahiye.
- Kuch MTP mounts local filesystem path nahi dete (sirf `mtp://...` GVFS URI), is soorat mein direct browse abhi supported nahi hai — status bar mein message aayega. Chahen to `gio open` / Nautilus se compare kar ke ye behtar kiya ja sakta hai.
- Audio thumbnail sirf tab milega jab file mein embedded cover art ho, warna generic audio icon dikhega.
- Ye ek functional base hai — multi-select bulk actions, aur properties dialog jaisi cheezein aap aage extend kar sakte hain.
- Image thumbnails ab file ke actual content (magic bytes) se detect hote hain, sirf extension se nahin — is liye bina extension wali ya "wrong" extension wali image files ka bhi thumbnail ban jayega (SVG ke liye system mein `librsvg`/gdk-pixbuf SVG loader installed hona chahiye).
- Bookmarks `~/.config/gtkfm/bookmarks` mein save hote hain.
