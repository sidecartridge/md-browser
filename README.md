# SidecarTridge Multi-device File & Download Manager

This microfirmware app for the **SidecarTridge Multi-device** lets you browse and download floppy images from the public database, manage files on your microSD card, and work directly with Atari ST disk images from a simple web interface.

## 🚀 Installation

To install the File & Download Manager app on your SidecarTridge Multi-device:

1. **Launch the Booster App** on your SidecarTridge Multi-device.
2. Open the **Booster web interface** in your browser.
3. Go to the **Apps** tab and select **File & Download Manager** from the available apps list.
4. Click **Download** to install the app to your SidecarTridge’s microSD card.
5. Once installed, select the app and click **Launch** to start it.

> **⚠️ WARNING:** The Booster Loader is currently in **alpha**. Use at your own risk.

After launching, the File & Download Manager app will run automatically every time your Atari is powered on.

## 🕹️ Usage

When you start your Atari, the app displays a screen with a **QR code**. Scan this QR code with your smartphone or tablet to access the web interface, or enter the provided URL in any browser.

* To **bypass** the QR code screen and boot directly into GEMDOS, press any key.
* To **return to the Booster App** and select another app, press the **ESC** key.

### 💾 Floppy Images Database

Click **Floppy DB** in the web interface to open the public floppy image database.

This page is split into two parts:

* **What’s New** for the latest uploads
* **Main Catalog** for older classics and crew collections

![Floppy Images Database Search Page](/BROWSER-FLOPPYDB-MAIN.png)

Use the tools at the top of the page to find what you want:

* **Search box:** Type part of a title to narrow the list
* **Label chips:** Filter by team name, software type, or collection label
* **What’s New:** Best for recent uploads
* **Main Catalog:** Best for older favorites

You can use the green **download** icon next to an entry to choose where it should be saved on your microSD card.

![Floppy Images Database Search Results](/BROWSER-FLOPPYDB-RESULT.png)

To download:

1. Click the **download icon** next to the file you want.
2. Choose a destination folder from the dialog.
3. Click **Download Here** to start the download.

When complete, your file is saved to the chosen folder. Use the [Drives Emulator](https://docs.sidecartridge.com/sidecartridge-multidevice/microfirmwares/drives_emulator/) or a solution like Gotek to access your files on your Atari.

### 🗂️ File Manager

#### Overview

The **File Manager** lets you manage files and folders on your microSD card. You can:

* Navigate folders
* View file details
* Rename, copy, move, and delete files or folders
* Upload multiple files
* Download files
* Create folders
* Upload files from internet URLs
* Create blank Atari ST disk images
* Convert `.MSA` and `.ST` floppy images
* Open and browse `.ST` and `.st.rw` images like folders

#### Table View

Click **File Manager** in the web interface. You’ll see your microSD card’s contents in a table with columns for:

* **Name**
* **Size**
* **Timestamp**
* **Actions** (icons for copy, move, rename, delete, and more)

Toolbar options above the table:

* **Upload files**: Select files from your computer to upload (multi-select supported)
* **Upload from URL**: Download files directly from an internet URL to the current folder
* **New Folder**: Create a new directory
* **Blank ST Image**: Create a new empty Atari ST disk image

![File Manager Table View](/BROWSER-FILEMANAGER-ROOT.png)

#### Row Colors

Rows are color-coded:

* **Black:** Standard files and folders
* **Gray:** Hidden files or folders
* **Red:** Read-only files

#### Navigation

* Click a folder row to enter it and see its contents.
* Click **..** at the top to go back to the parent folder.

![File Manager Navigation](/BROWSER-FILEMANAGER-NAVIGATION.png)

#### File Actions

Use the action icons on each row to manage files and folders. Click a file row for more details and options.

Depending on the file, you can:

* **Toggle hidden** attribute
* **Toggle read-only** attribute
* **Rename** file
* **Delete** file (with confirmation)
* **Download** file to your computer
* **Copy** to another folder
* **Move** to another folder
* **Convert** between `.MSA` and `.ST`
* **Browse Image** for `.ST` and `.st.rw` files

![File Details Actions](/BROWSER-FILEMANAGER-FILEACTIONS.png)

#### Creating a Blank ST Image

To create a new empty floppy image:

1. Open **File Manager**
2. Go to the folder where you want to save it
3. Click **Blank ST Image**
4. Choose the disk size
5. Enter a file name
6. Optionally enter a volume name
7. Choose `.st` or `.st.rw`
8. Click **Create image**

Use `.st` for a normal floppy image. Use `.st.rw` if you want to edit the image later inside the web interface or with the Multidrive Emulator.

#### Converting MSA and ST Images

If you have an `.MSA` or `.ST` file, use the **convert** icon to create the other format automatically.

This is useful when you download software in one format and need the other one for your Atari setup.

#### Browsing Inside ST Images

You can open `.ST` and `.st.rw` images directly in the file manager.

1. Find the image file
2. Click **Browse Image**
3. The image opens like a normal folder

Inside the image you can look through the files and folders without extracting the image first.

If the image is a writable `.st.rw` image, you can also:

* Rename files
* Delete files or folders
* Import files and folders from the microSD card into the image

You can also copy files or folders from the image back to the microSD card.

#### Uploading Files

Click **Upload files** to select and upload files from your computer (multiple at once). The upload dialog displays progress bars for each file. New files appear automatically in the table once uploaded.

![File Upload Dialog](/BROWSER-FILEMANAGER-UPLOADFILES.png)
![File Upload Done](/BROWSER-FILEMANAGER-UPLOADEDFILES.png)

### 🔌 USB Mass Storage Mode

You can also use your SidecarTridge Multi-device as a USB mass storage device. Simply connect it to your computer via USB, and the microSD card will mount as a regular USB drive. The QR code screen will show a message indicating USB Mass Storage Mode is active.

> **Note:** You can use the web interface and USB mode simultaneously, but changes made in one mode may not show up immediately in the other. Refresh your browser or file explorer to see updates.

### ⏏️ Exiting to GEMDOS or Booster

* **ESC** on the QR code screen launches the **Booster** app.
* You can also return to **Booster** from the top menu in the web interface.
* **Any other key** exits the emulator and boots into the Atari GEM desktop. The web interface remains active.
* To return to setup, press **SELECT** on your Multi-device and reboot, or power cycle your Atari.
* **Hold SELECT for 10 seconds** to reset your Multi-device to factory settings (useful for troubleshooting or starting fresh).

### ♻️ System Reset Behavior

The Drives Emulator app is **resistant to system resets**. Pressing the Atari reset button will **not interrupt downloads or file management**; your session continues seamlessly.

### 🔄 Power Cycling

After a power cycle, the app automatically returns to the QR code screen, ready for the next session.

### ⚙️ Advanced Features

#### Changing the microSD Card Speed

The SPI bus speed (1 MHz – 24 MHz; default is 12.5 MHz) affects all microSD access and is configured in the Booster App:

1. Launch the **Booster App** (press **X** if you’re in another app).
2. Open the web interface and go to the **Config** tab.
3. Find **SD card bus speed (KHz)** and set your desired speed (e.g., `24000` for 24 MHz).
4. Click **Save** to apply.
5. Relaunch the **Drives Emulator** app.

> **Note:** Values below 1 MHz will default to 1 MHz; above 24 MHz will default to 24 MHz.
>
> **Tip:** 24 MHz is generally safe, but lower the speed if you encounter issues (e.g., 12.5 MHz or 6 MHz). Most modern microSD cards handle these speeds well.

## 📜 License

The source code is licensed under the [GNU General Public License v3.0](LICENSE).

## 🤝 Contributing
Made with ❤️ by [SidecarTridge](https://sidecartridge.com)
