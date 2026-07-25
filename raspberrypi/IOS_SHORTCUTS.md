# iOS Shortcuts Setup for TZT

Send messages, images, and voice memos to the TZT from your iPhone or iPad.

## Password (HTTP Basic Auth)

| When | Password needed? |
|------|------------------|
| **Tailscale** (100.x.x.x) | No |
| **Same Wi‑Fi** (192.168.x.x) | No |
| **Cellular / public IP** | **Yes** – add header below |

If you get **401 Unauthorized** when on cellular, add this header in “Get Contents of URL”:
- **Name**: `Authorization`
- **Value**: `Basic ` + base64 of `username:password`

Example: for `admin` / `mypass`, encode `admin:mypass` to base64 and use `Basic YWRtaW46bXlwYXNz`.

## Pi IP addresses

| IP | When to use |
|----|-------------|
| **192.168.1.146** | Same Wi‑Fi as the Pi (home network) |
| **100.125.112.18** | Tailscale – from anywhere (install Tailscale app on iPhone) |

Use port **5000** when calling Flask directly: `http://IP:5000`  
If nginx is proxying (setup.sh), use port **80**: `http://IP`

---

## 1. Send text message

1. Open **Shortcuts** → **+** (New Shortcut).

2. **Ask for Input**
   - Add action → search “Ask for Input”
   - Prompt: `What message do you want to send?`
   - Input Type: **Text**

3. **Dictionary**
   - Add action → search “Dictionary”
   - Add keys:
     - `type` → **Text** → `print`
     - `data` → **Provided Input** (from step 1)
     - `source` → **Text** → `shortcut`

4. **Get Contents of URL**
   - Add action → search “Get Contents of URL”
   - **URL**: `http://100.125.112.18:5000/api/commands`  
     (or `http://192.168.1.146:5000` when on same Wi‑Fi)
   - Tap **Show More**
   - **Method**: **POST**
   - **Headers**: add  
     - Name: `Content-Type`  
     - Value: `application/json`
   - **Request Body**: **JSON** → select the **Dictionary** from step 2  
     (If the key says “Dictionary”, pick that and set it to the dictionary from step 2. The server also accepts the body if Shortcuts sends it in an alternate form.)

5. (Optional) **Show Notification** → Text: `Message sent!`

6. Rename shortcut to **Send TZT Message**.

---

## 2. Send image

1. New Shortcut → **+**

2. **Select Photos** (or **Take Photo**)
   - Add action → search “Select Photos”

3. **Get Contents of URL**
   - Add action → search “Get Contents of URL”
   - **URL**: `http://100.125.112.18:5000/api/images`
   - **Method**: **POST**
   - **Request Body**: **File** → select **Photos** (from step 1)

4. Rename shortcut to **Send TZT Image**.

> If upload fails, try the web UI: open `http://100.125.112.18:5000/` in Safari. The server now also accepts raw file uploads (body = file bytes) for Shortcuts.

---

## 3. Send voice memo

1. New Shortcut → **+**

2. **Record Audio**
   - Add action → search “Record Audio”  
   - (Or “Choose from Photos” for existing recording)

3. **Get Contents of URL**
   - Add action → search “Get Contents of URL”
   - **URL**: `http://100.125.112.18:5000/api/audio`
   - **Method**: **POST**
   - **Request Body**: **File** → select **Recorded Audio**

4. Rename shortcut to **Send TZT Voice**.

---

## 4. Main shortcut (Choose from Menu)

One shortcut that shows a menu and runs the right one. Create this **after** you have **Send TZT Message**, **Send TZT Image**, and **Send TZT Voice** (sections 1–3).

1. Open **Shortcuts** → **+** (New Shortcut).

2. **Choose from Menu**
   - Add action → search “Choose from Menu”
   - **Prompt**: `Send to TZT`
   - Tap **Add Option** three times and set the option **names** to exactly:
     - `Message`
     - `Photo`
     - `Audio`  
   (The menu will output the chosen name as text.)

3. **If** (run the right shortcut from the choice)
   - Add action → search “If”
   - **Input**: **Chosen Item** (from the Choose from Menu step)
   - **Condition**: **is** **Message**
   - Inside **If**: add **Run Shortcut** → **Send TZT Message**
   - After the **If** block: add **Otherwise, If**
   - **Condition**: **is** **Photo**
   - Inside: add **Run Shortcut** → **Send TZT Image**
   - After that: add **Otherwise**
   - Inside **Otherwise**: add **Run Shortcut** → **Send TZT Voice**

4. Rename the shortcut to **Send to TZT**.

When you run it, you pick Message, Photo, or Audio; the shortcut then runs Send TZT Message, Send TZT Image, or Send TZT Voice.

---

## Add to Home Screen

1. Long‑press a shortcut in the Shortcuts app
2. **Add to Home Screen**
3. Choose a name (e.g. “Send Message”) → **Add**

---

## URL reference

| Action   | URL                                      |
|----------|------------------------------------------|
| Text     | `http://100.125.112.18:5000/api/commands` |
| Image    | `http://100.125.112.18:5000/api/images`   |
| Voice    | `http://100.125.112.18:5000/api/audio`    |
| Web UI   | `http://100.125.112.18:5000/`             |

For local network, replace `100.125.112.18` with `192.168.1.146`.

---

## Troubleshooting

| Issue            | Fix                                                         |
|------------------|-------------------------------------------------------------|
| Could not connect| Check Pi is on, URL is correct, and Tailscale is running    |
| 401 / 403        | On cellular: add `Authorization: Basic base64(user:pass)` header |
| Image/audio fail | Use Tailscale IP (no auth). Or use web UI at http://IP/     |
| Message works, media doesn’t | Use Tailscale when possible; auth + file upload can conflict |
