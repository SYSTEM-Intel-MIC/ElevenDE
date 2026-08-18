#pragma once
/* Extra Settings pages for ElevenDE (declared here, defined in extra.cpp).
 * Each function returns a ready-to-use page widget, styled by the shared
 * Win11 stylesheet. */

class QWidget;

namespace ElevenSettings {

QWidget *buildSoundPage();
QWidget *buildDateTimePage();
QWidget *buildDefaultAppsPage();
QWidget *buildMousePage();
QWidget *buildPowerPage();
QWidget *buildUsersPage();

} // namespace ElevenSettings
