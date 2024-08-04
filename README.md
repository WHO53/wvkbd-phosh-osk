### Steps:

1. Clone the repository:
    ```sh
    git clone https://github.com/WHO53/wvkbd-phosh-osk
    ```

2. Copy the `wvkbd-dbus` binary to `/usr/bin`:
    ```sh
    sudo cp wvkbd-phosh-osk/wvkbd-dbus /usr/bin/
    ```

3. Add `wvkbd-dbus.desktop` file to `.config/autostart`:
    ```sh
    mkdir -p ~/.config/autostart
    cp wvkbd-phosh-osk/wvkbd-dbus.desktop ~/.config/autostart/
    ```

4. Edit `/usr/share/applications/sm.puri.Squeekboard.desktop`:
    ```sh
    sudo vi /usr/share/applications/sm.puri.Squeekboard.desktop
    ```
    Change the `Exec` line to:
    ```
    Exec=/usr/bin/wvkbd-mobintl
    ```

    Remove the following three lines:
    ```
    X-Phosh-UsesFeedback=true
    X-GNOME-Autostart-Phase=Panel
    X-GNOME-Autostart-Notify=true
    ```

5. Reboot the system:
    ```sh
    sudo reboot
    ```
