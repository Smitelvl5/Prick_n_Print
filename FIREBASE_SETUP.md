# Firebase Realtime Database setup

If the TZT shows **"Firebase: Unauthorized"**, the database is rejecting requests because of security rules. Fix it as follows.

**Sending images from an iOS Shortcut?** See **[SHORTCUTS_IMAGE_UPLOAD.md](SHORTCUTS_IMAGE_UPLOAD.md)** for step-by-step instructions (Firebase Storage upload + Realtime Database) for this project.

## 1. Open your project in Firebase Console

1. Go to [Firebase Console](https://console.firebase.google.com/).
2. Select your project (or create one).

## 2. Open Realtime Database rules

1. In the left sidebar, click **Build** → **Realtime Database**.
2. If you see “Create Database”, create it (choose a region, then start in **test mode** or **locked mode**; we’ll set rules next).
3. Click the **Rules** tab at the top.

## 3. Set rules so the TZT can use `/commands`

The TZT only needs read and write access to **`/commands`**. Copy **only** the JSON (no \`\`\` markers) into the Firebase Rules editor, then pick one of the following.

**Minimal (only commands):**

```json
{
  "rules": {
    "commands": {
      ".read": true,
      ".write": true
    }
  }
}
```

**If you also use other paths (e.g. web UI):**

Copy **only** the JSON below (no \`\`\` markers) into the Firebase Rules editor:

```json
{
  "rules": {
    "commands": {
      ".read": true,
      ".write": true
    },
    "config": { ".read": true, ".write": true },
    "status": { ".read": true, ".write": true },
    "reminders": { ".read": true, ".write": true },
    "groceries": { ".read": true, ".write": true },
    "todos": { ".read": true, ".write": true },
    "audio": { ".read": true, ".write": true },
    "images": { ".read": true, ".write": true }
  }
}
```

## 4. Publish the rules

1. Click **Publish**.
2. Wait a few seconds for the new rules to apply.

## 5. Check the database URL

1. In **Realtime Database**, at the top you’ll see the database URL, e.g.  
   `https://YOUR-PROJECT-ID-default-rtdb.firebaseio.com`
2. In your project, set this same URL in:
   - **`include/config_tzt.h`**: `#define FIREBASE_DATABASE_URL "https://YOUR-PROJECT-ID-default-rtdb.firebaseio.com"`
   - Or when running the test script:  
     `python scripts/test_tzt_display.py --firebase-url "https://YOUR-PROJECT-ID-default-rtdb.firebaseio.com"`

## 6. Retry the TZT

Restart the TZT or wait for the next Firebase poll.

## 7. (Optional) Firebase Storage — for image uploads

If you use the **Shortcut** or **`scripts/upload_image_to_firebase.py`** to send images to the TZT, enable Storage or you'll get **404** on upload: **Build** → **Storage** → **Get started** → choose test mode and location → **Done**. Then run the upload script again. The “Unauthorized” errors should stop once the rules allow read/write on `commands` and the URL matches your project.

---

**Note:** These rules allow anyone with the database URL to read and write. For a personal device on your network this is often acceptable. For a production app you would use Firebase Authentication and restrict rules by `auth != null` or similar.
