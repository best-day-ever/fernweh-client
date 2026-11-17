# Display setup interactions

This suite loads the real QML dialog, controller, planner and isolated QSettings.
It checks resolution selection, Save/reopen, focused dimension edits, validation,
Cancel, host capability gating, standard-DPI sizing and screen/primary choices.
The monitor probe, decoder limit and broker calls are fixtures; no workstation,
user profile, GPU, network connection or capture session is used.

Run the qmake target with `QT_QPA_PLATFORM=offscreen` and
`QT_QUICK_BACKEND=software`. Optionally set `PLANK_DISPLAY_SETUP_CAPTURE_DIR`
to an output directory for synthetic UI screenshots. Hardware streaming and
native package acceptance remain separate checks.
