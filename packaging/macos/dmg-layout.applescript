-- Configure only the mounted image's window. Other Finder windows stay open.
on run argv
    set mountPath to item 1 of argv
    set actionName to item 2 of argv
    set imageAlias to POSIX file mountPath as alias
    tell application "Finder"
        set imageFolder to folder imageAlias
        open imageFolder
        set imageWindow to container window of imageFolder
        if actionName is "style" then
            set current view of imageWindow to icon view
            set toolbar visible of imageWindow to false
            set statusbar visible of imageWindow to false
            set bounds of imageWindow to {100, 100, 740, 528}
            set viewOptions to icon view options of imageWindow
            set arrangement of viewOptions to not arranged
            set icon size of viewOptions to 128
            set text size of viewOptions to 14
            set background picture of viewOptions to file ".background:background.png" of imageFolder
            set position of item "KiCad Forge.app" of imageFolder to {160, 190}
            set position of item "Applications" of imageFolder to {480, 190}
            update imageFolder without registering applications
            delay 1
        else if actionName is "verify" then
            set viewOptions to icon view options of imageWindow
            if current view of imageWindow is not icon view then error "DMG does not open in icon view"
            if toolbar visible of imageWindow then error "DMG toolbar was not hidden"
            if statusbar visible of imageWindow then error "DMG status bar was not hidden"
            if bounds of imageWindow is not {100, 100, 740, 528} then error "DMG window bounds were not saved"
            if (arrangement of viewOptions) is not (not arranged) then error "DMG icon arrangement was not saved"
            if icon size of viewOptions is not 128 then error "DMG icon size was not saved"
            if position of item "KiCad Forge.app" of imageFolder is not {160, 190} then error "DMG app position was not saved"
            if position of item "Applications" of imageFolder is not {480, 190} then error "DMG Applications position was not saved"
            -- Finder cannot always coerce its background-picture reference to
            -- an alias after the image is remounted read-only. Compare the
            -- files' native URLs instead, resolving both through Finder.
            set expectedBackgroundURL to URL of file ".background:background.png" of imageFolder
            set savedBackground to get background picture of viewOptions
            log {"DMG saved background", savedBackground}
            set actualBackgroundURL to URL of savedBackground
            if actualBackgroundURL is not expectedBackgroundURL then error "DMG background was not saved"
        else
            error "Unknown DMG layout action"
        end if
        close imageWindow
    end tell
end run
