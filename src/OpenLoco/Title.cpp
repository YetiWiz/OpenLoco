#include "Title.h"
#include "Audio/Audio.h"
#include "CompanyManager.h"
#include "Gui.h"
#include "Interop/Interop.hpp"
#include "OpenLoco.h"
#include "Scenario.h"
#include "Ui/WindowManager.h"

using namespace OpenLoco::Interop;

namespace OpenLoco::Title
{
    static void sub_473A95(int32_t eax);

    void registerHooks()
    {
        registerHook(
            0x0046AD7D,
            [](registers& regs) FORCE_ALIGN_ARG_POINTER -> uint8_t {
                start();
                return 0;
            });
    }

    // 0x00472031
    // ?unload all objects?
    static void sub_472031()
    {
        call(0x00472031);
    }

    // 0x00474874
    // ?load selected objects?
    static void sub_474874()
    {
        call(0x00474874);
    }

    // 0x00473B91
    // object flags free 0
    static void sub_473B91()
    {
        call(0x00473B91);
    }

    // 0x004284C8
    void sub_4284C8()
    {
        call(0x004284C8);
    }

    // 0x00444357
    static void sub_444357()
    {
        call(0x00444357);
    }

    // 0x0046AD7D
    void start()
    {
        CompanyManager::updatingCompanyId(CompanyManager::getControllingId());
        if (isPaused())
        {
            togglePause(true);
        }

        auto currentScreenFlags = getScreenFlags();
        clearScreenFlag(ScreenFlags::networked);
        Ui::WindowManager::closeAllFloatingWindows();
        setAllScreenFlags(currentScreenFlags);
        setScreenFlag(ScreenFlags::title);
        setGameSpeed(0);
        sub_472031();
        sub_473A95(1);
        sub_474874();
        sub_473B91();
        ObjectManager::reloadAll();
        Scenario::sub_4748D4();
        Scenario::reset();
        initialiseViewports();
        sub_4284C8();
        Gui::init();
        sub_444357();
        Gfx::invalidateScreen();
        resetScreenAge();

        Audio::playTitleScreenMusic();
    }

    // 0x00473A95
    static void sub_473A95(int32_t eax)
    {
        registers regs;
        regs.eax = eax;
        call(0x00473A95, regs);
    }
}
