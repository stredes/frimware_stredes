#ifndef __C2_MENU_H__
#define __C2_MENU_H__

#include <MenuItemInterface.h>

class C2Menu : public MenuItemInterface {
public:
    C2Menu() : MenuItemInterface("C2") {}

    void optionsMenu(void);
    void drawIcon(float scale);
    bool hasTheme() { return false; }
    String themePath() { return ""; }

private:
    void connectMenu();
    void configMenu();
    void showServerStatus();
    void pingAgent();
    void openSSH();
    void openTcpClient();
    void openHttpHealth();
};

#endif
