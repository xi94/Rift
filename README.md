# Rift

A Riot account manager for Windows. Store your accounts behind one master password, pick a game, click login — Rift launches the Riot Client, signs in, and takes you into the game.

https://github.com/user-attachments/assets/0f34ccb4-8154-4b15-878b-d3f58a68f2d2

## Features

- **One-click login** — launches the Riot Client, fills the form, and presses Play for you
- **Tray quick-login** — right-click the tray icon to sign in without opening the window; minimizing hides Rift there
- **Encrypted vault** — accounts stored encrypted, master password derived with Argon2
- **Anti screen-capture** — the window won't show up in screenshares or screenshots
- **Three views** — Ctrl+Scroll to zoom between Carousel, Grid, and List
- **Won't interrupt a live match** — refuses to run while VALORANT or League is open

Supports League of Legends, Teamfight Tactics, Valorant, 2XKO, and Legends of Runeterra.

## How it works

Rift drives the Riot Client through UI Automation, the same Windows accessibility API a screen reader uses. It finds the login fields and fills them in exactly as you would.

No memory reading, no injection, no hooking. Credentials never leave your machine — they go into the Riot Client's own login form and nowhere else.

## Getting started

1. Set a master password on first launch. There's no recovery — forget it and the vault is gone.
2. Add your accounts.
3. Pick a game, pick an account, hit Login.

Requires Windows 10/11 and the Riot Client installed.

## Caveats

Rift matches on Riot Client UI element names, so a client update or a non-English locale can break login until they're updated.

Automating the Riot Client may not sit well with Riot's Terms of Service. Rift doesn't touch game processes or provide any in-game advantage, but the account risk is yours to weigh.
