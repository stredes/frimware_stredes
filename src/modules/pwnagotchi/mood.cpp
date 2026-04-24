/*
Thanks to thoses developers for their projects:
* @7h30th3r0n3 : https://github.com/7h30th3r0n3/Evil-M5Core2 and https://github.com/7h30th3r0n3/PwnGridSpam
* @viniciusbo : https://github.com/viniciusbo/m5-palnagotchi
* @sduenasg : https://github.com/sduenasg/pio_palnagotchi

Thanks to @bmorcelli for his help doing a better code.
*/
#if !defined(LITE_VERSION)
#include "mood.h"

// ASCII equivalent
const String palnagotchi_moods[] = {
    "(v__v)", // 0 - sleeping
    "(=__=)", // 1 - awakening
    "(O__O)", // 2 - awake
    "( O_O)", // 3 - observing (neutral) right
    "(O_O )", // 4 - observig (neutral) left
    "( 0_0)", // 5 - observing (happy) right
    "(0_0 )", // 6 - observing (happy) left
    "(+__+)", // 7 - intense
    "(-@_@)", // 8 - cool
    "(0__0)", // 9 - happy
    "(^__^)", // 10 - grateful
    "(UwU )", // 11 - excited
    "(+__+)", // 12 - smart
    "(*__*)", // 13 - friendly
    "(@__@)", // 14 - motivated
    "(>__<)", // 15 - demotivated
    "(-__-)", // 16 - bored
    "(T_T )", // 17 - sad
    "(;__;)", // 18 - lonely
    "(X__X)", // 19 - broken
    "(#__#)", // 20 - debugging
    "(-_-')", // 21 - angry
    "(^__^)", // 22- helping
};

const String palnagotchi_moods_desc[] = {
    "Durmiendo...",                        // 0 - sleeping
    "...",                                  // 1 - awakening
    "Quiero hacer amigos ya.",              // 2 - awake
    "Buscando amigos...",                   // 3 - observing (neutral) right
    "Buscando amigos...",                   // 4 - observig (neutral) left
    "Rastreando colegas que hagan pwn.",    // 5 - observing (happy) right
    "Rastreando colegas que hagan pwn.",    // 6 - observing (happy) left
    "Si, hay muchos pwnagotchis!",          // 7 - intense
    "El colega mas frio del barrio.",       // 8 - cool
    "Podemos conseguir mas amigos?",        // 9 - happy
    "Me encantan los pwnagotchis.",         // 10 - grateful
    "De grande voy a pwnear solo.",         // 11 - excited
    "3.1415926535897932384626433832795",    // 12 - smart
    "Eh tu, seamos amigos.",                // 13 - friendly
    "Corre perfecto. PWND.",                // 14 - motivated
    "Si no pwn, no existo.",                // 15 - demotivated
    "Necesito salir a cazar redes.",        // 16 - bored
    "Este hardware hoy no puede pwn.",      // 17 - sad
    "Donde estan los pwnagotchis?",         // 18 - lonely
    "En mi lado si funciona.",              // 19 - broken
    "Mis amigos si pwn.",                   // 20 - debugging
    "Muy enojado, pero sin pwn.",           // 21 - angry
    "Ayudando pwnagotchis desde 2024",
};

uint8_t current_mood = 0;
String current_phrase = "";
String current_face = "";
bool current_broken = false;
const int number_of_moods = 23; // sizeof(palnagotchi_moods);

uint8_t getCurrentMoodId() { return current_mood; }
int getNumberOfMoods() { return number_of_moods; }
String getCurrentMoodFace() { return current_face; }
String getCurrentMoodPhrase() { return current_phrase; }
bool isCurrentMoodBroken() { return current_broken; }

void setMood(uint8_t mood, String face, String phrase, bool broken) {
    current_mood = mood;
    current_broken = broken;

    if (face != "") {
        current_face = face;
    } else {
        current_face = palnagotchi_moods[current_mood];
    }

    if (phrase != "") {
        current_phrase = phrase;
    } else {
        current_phrase = palnagotchi_moods_desc[current_mood];
    }
}
#endif
