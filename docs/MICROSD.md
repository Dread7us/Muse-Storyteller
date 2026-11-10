# Removable storage on Module LLM

Insert a FAT32 or exFAT card into the **Module LLM** slot and open **Settings → microSD**. The page uses the Module’s verified automatic mount when present. Tap **Mount** if the card is detected but not mounted. The page shows free space, storage usage, and the current folder. Tap an item to open it, rename it, move it, or confirm removal from the card. For a move, open the destination folder and tap **Move here**. The explorer previews the start of text files. Use **New folder** to organize the card and **Up**, **Previous**, and **Next** to navigate.

**Export all** copies completed Recorder sessions and a conversation backup into a new folder under `Muse Exports`. Use **Recordings** for compressed WAV chunks plus their transcripts, **Transcripts** for the text only, or **Conversations** for a consistent `conversations.sqlite3` snapshot. **Export to microSD** in Recorder limits recording and transcript copies to the selected session. The original recording, raw transcript, and live conversation library stay on the Module. Copied files and conversations may contain private information.

Keep the card inserted until the progress indicator finishes. Tap **Eject** before removing it or putting it in a PC card reader. Before formatting, copy any files you want to keep to another drive and verify the copies. With a FAT formatter installed on the Module, **Format as FAT32** appears after ejecting the card. Confirm the erase, then tap **Mount** to open the newly labeled `MUSE_SD` card. The formatter acts on the verified removable card's single data volume and leaves its partition layout alone.

## Copy over USB

With the card mounted, use the USB copy helper from a PC that already has an ADB connection to the Module. It only targets `axera-ax620e` and copies an item from the mounted card. Use `--adb` and `--port` for a dedicated diagnostic connection.

```sh
python tools/copy_microsd.py --item "Muse Exports" /path/to/destination
```

Choose a specific file or folder by giving its path relative to the card root. The Module’s stock USB connection stays in ADB mode. A card reader gives ordinary drive-letter access after ejecting the card.
