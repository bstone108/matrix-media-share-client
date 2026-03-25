# Matrix Media Share Client User Guide

This guide explains what the app does, what each page is for, what the main settings mean, and a few important quirks to know while testing.

The goal of the app is simple:

- browse media shared in Matrix rooms
- download only what you want
- share files through Matrix and IPFS
- keep your own managed `Shared Files` folder
- optionally reuse an existing large `Archive Root` instead of storing duplicates

## What The App Does

Matrix Media Share Client is not a blind archive bot. It is a desktop client that:

- watches joined rooms for media
- shows that media in a browser
- lets you open or download selected items
- can share local files to rooms
- uses IPFS for large files and long-term availability
- still uses normal Matrix uploads when the file is small enough

For each share, the app tries to give people an easy path:

- if the file is small enough for Matrix, it uploads the media to Matrix too
- if the file is too large, it uploads a preview thumbnail to Matrix instead
- either way, it also publishes the media to IPFS
- the Matrix post includes an IPFS landing page link so people can open it in a browser

## Main Pages

## Browser

This is the main page and the normal starting point.

Use it to:

- pick a room from the left sidebar
- browse media found in that room
- double-click an item to open it in the built-in viewer
- click `Download Selected` to add it to the normal download queue
- click `Upload Files` or drag and drop files onto the window to share them into the selected room

Notes:

- the selected room in Browser is the target room for uploads
- only joined rooms can be uploaded to
- cached rooms can still be browsed, but they are not upload targets until you join them

## Rooms

Use this page to manage rooms and spaces.

Use it to:

- see joined rooms
- see rooms discovered from spaces
- join rooms
- leave rooms

Important:

- the app does not auto-join everything by default
- Browser and Rooms share the room sidebar on the left

## Shared Files

This page is for the app-managed files it is actively sharing.

Think of it as:

- a visual page of shared items with thumbnails
- a quick place to open shared items in the built-in viewer
- a management page for the app’s own shared copies

Important:

- deleting from this page only removes the app-managed copy in `Shared Files Root`
- it does not delete matching files from `Downloads Root`
- it does not delete anything from `Archive Root`
- the archive is always treated as your folder, not the app’s folder

## Transfers

This page shows queued and active downloads.

Use it to:

- see what is downloading now
- see what is waiting
- see failed items
- retry failed items

The built-in viewer download is special:

- opening media for viewing skips the normal download queue
- it downloads directly for the viewer so it can start as soon as possible
- it still shows progress in the UI while it downloads

## Logs

This page is your first stop when something seems wrong.

Use it to:

- see recent app activity
- see warnings and errors
- confirm thumbnail caching activity
- confirm upload, viewer, room refresh, and IPFS actions

The app now favors logging over popup spam, so most problems should appear here instead of interrupting you with endless dialog boxes.

## Settings

This page is where you point the app at your homeserver, folders, and sharing preferences.

The page scrolls because there are a lot of options.

## Verification

This page is for Matrix device verification.

Use it when you want to verify this client with another device on the same account.

What it does:

- requests verification from another logged-in device
- supports SAS verification when the other device supports it
- shows emoji or decimal verification codes when available

## Folders And What They Mean

The app uses several different folders. They do different jobs.

## Destination Root

This is where normal room downloads go.

In simple terms:

- if you browse a room and choose `Download Selected`
- the saved file goes under `Destination Root`

By default, the app uses subfolders so room media is organized. If you turn on `Flat Folder Layout`, files go directly into the root instead.

Important:

- the app does not create a room folder here until something is actually downloaded for that room

## Shared Files Root

This is the app’s own managed share storage.

Use this when:

- you upload/share a file through the client
- the file needs a managed local copy

What happens:

- normally, a shared file is copied here so the app can keep serving it
- if the same file is later found in your archive, the app can stop keeping the duplicate here and use the archive copy instead

## Archive Root

This is optional.

Use it if you already have a very large media collection somewhere else, such as:

- an external drive
- a NAS
- another big organized folder tree

The app does not care how your archive is arranged.

It works by content hash:

- if the exact same file already exists in your archive
- the app can track that match
- then it can use the archive copy instead of keeping a duplicate in `Shared Files Root`

This is mainly for uploads and long-term sharing.

## Downloads Root

This is the single final download location for regular downloads and manual IPFS imports.

In simple terms:

- if you download something from a room, it goes here
- if you paste an IPFS link manually, it also goes here
- IPFS downloads follow the same destination rules as regular downloads

## Folders The App Manages Automatically

You do not need to set these manually.

The app also keeps its own internal support folders for:

- the settings database
- secrets storage
- temporary viewer downloads
- cached thumbnails
- generated IPFS landing pages and other managed share resources
- bundled Kubo/IPFS data

Those are managed automatically under the app’s support path.

## Settings Explained

## Homeserver

Your Matrix homeserver URL.

Example:

- `https://matrix.org`
- your own self-hosted server URL

## Username

Your Matrix login name.

This can be:

- a full Matrix ID like `@name:server`
- or the localpart if that is how you log in

## Password

Your Matrix account password.

The app uses it for login and, when needed, for verification setup.

## Destination Root

Where normal room downloads are saved.

## Shared Files Root

Where the app keeps managed copies of files it is sharing.

How it is organized:

- the app manages this folder automatically
- each media type gets its own folder
- inside that is a subfolder named after the file hash
- thumbnails, landing pages, and related share resources live inside that hash folder

Important:

- this is an app-managed folder
- the client may rearrange misplaced files inside it
- the client may purge stray files in it that do not belong to tracked shared items

## Archive Root

Optional external media library path for dedupe and long-term reuse.

## Downloads Root

Where regular downloads and manual IPFS imports are saved.

## Archive Scan Enabled

Turns archive scanning on or off.

When on:

- the app scans your archive in the background
- looks for exact hash matches
- can repoint shared files to your archive copy

## Archive Scan High Priority

Makes the archive scanner run faster.

Normal recommended use:

- leave this off

Why:

- the scanner is meant to be a very low-priority background job
- normal use should not need it to rush

## Flat Folder Layout

If on:

- downloads and shared files go directly into the chosen root
- the app does not create room/category subfolders there

If off:

- the app organizes files into subfolders for easier browsing

## Primary Gateway

This is the preferred public IPFS gateway used for landing-page links.

The app may still include alternate gateways in the generated landing page.

## Preferred Gateways

This is the list of alternate public gateways the app knows about for your landing pages.

The generated HTML page can link to these alternates so people have other options if one gateway is slow or down.

## Message Limit

This controls how much message history the client scans and handles in a pass.

If you are not sure:

- leave it alone

## Retry Cooldown (min)

How long the app waits before retrying a failed download job.

## Retry Limit

How many times the app should retry before marking a download as permanently failed.

## Download Workers

How many normal download jobs the app can work on at the same time.

This affects queued downloads, not the direct viewer-open path.

## Bandwidth Limit (KiB/s)

Upload/download rate limit setting.

If set to `0`, it means unlimited.

## Preview Workers

Reserved for preview-related work.

If you are not sure:

- leave this at the default

## Autostart

Tells the app whether it should start with the operating system.

This setting exists in the UI, but system integration work may still vary by platform while the app is in beta.

## Minimize To Tray

Tells the app to stay alive in the tray instead of fully shutting down when appropriate.

Important:

- backend helper processes should only stay alive while the app itself is running or intentionally living in the tray

## Start Hidden

Starts the app hidden instead of opening the full window right away.

## Auto Join Space Rooms

If enabled, the app can automatically join rooms discovered through spaces.

If disabled, you manage room joins yourself.

## Auto Download New Media

If enabled, newly discovered media can be queued automatically.

If disabled, browsing stays selective and manual.

## Self-Heal Shared Files

This is off by default.

When on:

- if a managed shared copy goes missing
- and the app knows how to recover it from IPFS
- it can re-download that managed copy at low priority so it can keep sharing it

Important:

- this only affects the app-managed `Shared Files Root`
- it does not write into `Archive Root`
- it does not take over your normal downloads

## Current Version / Update Status / Latest Release / Last Checked

These are for the built-in update checker.

How it works:

- the app checks GitHub releases about once a week
- if a check is overdue, it checks on startup
- the `Open Latest Release` button opens the release page in your browser

## Important Quirks And Behavior

## 10-Second Upload Timer

The upload/share queue waits about 10 seconds between queued shares.

Why this exists:

- Matrix homeservers often have upload limits and rate limits
- firing many uploads back-to-back is more likely to trip those limits
- spacing them out makes shares more reliable

What it means in practice:

- if you queue several files to share
- they go one at a time
- the next one waits a bit before posting

This is intentional, not a freeze.

## One Share Should Be One Message

The app now tries to keep a single shared file inside a single Matrix message/event whenever possible.

That means:

- if the file itself is uploaded to Matrix, the IPFS details go in the attachment comment/caption
- if the file is too large, the preview thumbnail and the IPFS details go together

The goal is to avoid cluttering the room with multiple follow-up messages for one file.

## Viewer Downloads Skip The Normal Queue

If you open media for viewing:

- it should not wait behind the normal download queue
- it downloads directly for the built-in viewer
- the viewer should show progress while it is fetching

This is different from `Download Selected`, which goes through the normal queue and saves the file to your download location.

## Thumbnail Downloads

The thumbnail system now works in two modes:

- foreground
  - thumbnails currently on screen
- background
  - nearby off-screen thumbnails for smoother scrolling

Foreground thumbnails should win first.

Background work should only run when the foreground queue is clear.

## Active Room Refresh

The room you are actively viewing should be refreshed in real time and treated as the priority room.

This helps with:

- seeing newly shared content faster
- warming thumbnails for the room you are actually using

## Archive Behavior

The app’s own managed sharing area is `Shared Files Root`.

If you also have an `Archive Root`:

- uploads can start by using a managed copy
- later, if the same exact file is found in the archive
- the app can remove the duplicate managed copy
- then keep using the archive copy instead

That helps save space.

## Shared Files Deletion

If you delete an item from the `Shared Files` page:

- the app removes the tracked shared item
- it cleans up the matching managed bundle from `Shared Files Root`

What it does not do:

- it does not delete anything from `Downloads Root`
- it does not delete anything from `Archive Root`
- it does not modify your archive

## IPFS Links Versus Raw Media

For people in a browser:

- the landing page is the friendly link

For the client itself:

- the client should download the raw media directly
- it should not waste time downloading the HTML landing page just to get the file

## Recommended First-Time Setup

1. Open `Settings`.
2. Enter your `Homeserver`, `Username`, and `Password`.
3. Pick a `Destination Root`.
4. Pick a `Shared Files Root`.
5. If you already have a huge library, set an `Archive Root`.
6. Save settings.
7. Turn power on.
8. Join the rooms you want from the `Rooms` page.
9. Browse media from the `Browser` page.

## Good Test Cases

If you are helping test the app, these are useful things to try:

- share one small file
- share one large file
- queue several uploads and watch the delay between them
- drag and drop files into Browser
- open an image in the built-in viewer
- open a video in the built-in viewer
- browse a room, restart the app, and confirm cached media still shows up
- enable archive scanning and confirm duplicate uploads can move to archive-backed storage
- check the `Logs` page when something behaves strangely

## If Something Looks Broken

Start with:

- `Logs` page
- `Transfers` page
- whether the room is actually joined
- whether IPFS says it is running

Common examples:

- no upload target
  - the selected Browser room may be cached, not joined
- slow sharing
  - you may be seeing the intentional 10-second share delay
- viewer does not open immediately
  - check whether it is downloading directly for viewing
- missing media previews
  - watch Logs for thumbnail cache activity

## Maintainer Note

Keep this file up to date whenever the user-facing behavior changes.

That includes changes to:

- page layout
- folder behavior
- sharing rules
- viewer behavior
- upload pacing
- settings labels
- update flow
- verification flow
