Colosseum Native for Xbox Series X|S (Developer Mode)
=====================================================

What you need
-------------
- An Xbox in Developer Mode (Dev Home is the home screen).
- A PC on the same network, for Xbox Device Portal.
- Your own Pokemon Colosseum (USA) disc image (ISO, GCM or CISO) on a USB drive.

Install
-------
1. On the Xbox, open Dev Home and note the Remote Access address (for example
   https://192.168.1.50:11443). Turn on "Remote Access" if it's off.
2. On the PC, open that address in a browser. Accept the certificate warning and sign in
   with the Device Portal user name and password you set in Dev Home.
3. Home > My games & apps > Add.
   - Choose ColosseumNative-Xbox.msix. Leave "Allow me to select framework packages"
     unticked (none are needed) and click Next / Start. Wait for "Package successfully
     registered".
   - If the portal asks for a certificate, add ColosseumNative.cer as well.
4. Still in My games & apps, open the "..." menu next to Colosseum Native > View details,
   and set the app type to Game. (As an "App" the Xbox only gives it a small share of the
   memory and graphics power, and the game will be slow or fail to start.)

First run
---------
5. Plug the USB drive with your disc image into the Xbox and start Colosseum Native from
   Dev Home (or My games & apps).
6. The setup screen lists disc images it found on USB drives. Pick yours with the D-pad and A,
   or choose "Browse for the disc image..." to find it in the Xbox file picker.
7. It checks the image (Pokemon Colosseum USA only) and unpacks it once, about 1.4 GB.
   The USB drive isn't needed after that.

Playing
-------
- Controller: same layout as the PC version. A/B/X/Y as on a GameCube pad, RB = Z,
  LT/RT = L/R, Menu = Start.
- View button: the in-game menu (graphics presets, frame rate, aspect ratio, save states,
  quit).
- B never exits the app; quit from the in-game menu, or with the Xbox button.

Saves and logs
--------------
Everything lives in the app's LocalState folder, which Device Portal's File explorer can
reach (LocalAppData > ColosseumNative.Xbox_... > LocalState):
- User\GC\MemoryCardA.USA.raw   the memory card (same format as the PC version; you can copy
                                your PC save here and back)
- User\Config                   settings
- ColosseumNative.log           start-up log; send it along if something goes wrong
- Game                          the unpacked disc (delete it to choose a disc again)
