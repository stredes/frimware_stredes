#ifndef __BRUCEGOTCHI_MENU_H__
#define __BRUCEGOTCHI_MENU_H__

#include <MenuItemInterface.h>

class BrucegotchiMenu : public MenuItemInterface {
public:
    BrucegotchiMenu() : MenuItemInterface("Brucegotchi") {}

    void optionsMenu(void);
    void drawIcon(float scale);
    bool hasTheme() { return false; }
    String themePath() { return ""; }
};

#endif
