#include <switch.h>
#include <stdio.h>

int main(int argc, char* argv[]) {
    consoleInit(NULL); // Initialize a simple text console on the default screen

    // Set up standard gamepad input (handles paired Joy-Cons or Pro Controller)
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    printf("Hello, Switch Homebrew!\\n");
    printf("Press + to exit.\\n");

    // Main loop: keep running until user presses + (HOME closes automatically too)
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus) break;

        consoleUpdate(NULL);
    }

    consoleExit(NULL);
    return 0;
}
