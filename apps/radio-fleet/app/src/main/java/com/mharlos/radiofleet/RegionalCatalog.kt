package com.mharlos.radiofleet

/** Rainier/Goble, OR and Longview/Kelso, WA regional listening plan.
 * Public directory assignments checked 2026-11-27. Reception and current
 * activity are not guaranteed; all entries are strictly receive-only.
 */
internal object RegionalCatalog {
    private fun rx(slot: Int, name: String, freq: Double, mode: String,
                   service: String, description: String) =
        Channel(slot, name, freq, null, narrow = mode == "NFM", power = "Low",
            mode = mode, service = service, description = description)

    // FAA airport communications records via AirNav:
    // https://www.airnav.com/airport/KKLS
    // https://www.airnav.com/airport/KPDX
    // https://www.airnav.com/airport/KSPB
    // https://www.airnav.com/airport/KAST
    val aviation = listOf(
        rx(20,"ZSE124",124.200,"AM","Local aviation","Seattle Center; Kelso/Scappoose and Astoria approach/departure"),
        rx(21,"PDXAPP",124.350,"AM","Local aviation","Portland approach/departure, 280–099 sector; Scappoose approach"),
        rx(22,"SPBDEP",133.000,"AM","Local aviation","Scappoose departure"),
        rx(23,"PDXHIGH",118.100,"AM","Local aviation","Portland high-sector approach/departure"),
        rx(24,"PDXFIN",126.900,"AM","Local aviation","Portland final approach"),
        rx(25,"PDXDEP",127.850,"AM","Local aviation","Portland departure"),
        rx(26,"KLSCTAF",122.725,"AM","Local aviation","Kelso/Longview airport CTAF/UNICOM"),
        rx(27,"SPBAST",122.800,"AM","Local aviation","Scappoose and Astoria airport CTAF/UNICOM"),
        rx(28,"PDXTWN",118.700,"AM","Local aviation","PDX tower runway 10L/28R"),
        rx(29,"PDXTWS",123.775,"AM","Local aviation","PDX tower runway 03/21 and 10R/28L"),
        rx(30,"PDXATIS",128.350,"AM","Local aviation","PDX D-ATIS"),
        rx(31,"AIRGARD",121.500,"AM","Local aviation","Civil aviation emergency guard")
    )

    // USCG channel table: https://navcen.uscg.gov/us-vhf-channel-information
    // River operators: https://www.pdxmex.com/services/marops/
    // https://mfsa.com/about-us/communications/
    // https://www.columbiariverbarpilots.com/contact/
    val marine = listOf(
        rx(32,"MAR13",156.650,"FM","Columbia marine","Channel 13 bridge-to-bridge navigation"),
        rx(33,"MAR16",156.800,"FM","Columbia marine","Channel 16 distress/calling; Merchants Exchange and pilots"),
        rx(34,"MAR11",156.550,"FM","Columbia marine","Channel 11 commercial traffic"),
        rx(35,"MAR14",156.700,"FM","Columbia marine","Channel 14 port operations"),
        rx(36,"MAR18A",156.900,"FM","Columbia marine","Channel 1018 / 18A commercial ship-to-shore"),
        rx(37,"MAR09",156.450,"FM","Columbia marine","Channel 9 calling; Columbia River Bar Pilots"),
        rx(38,"MAR22A",157.100,"FM","Columbia marine","Channel 1022 / 22A Coast Guard safety broadcasts"),
        rx(39,"MAR81A",157.075,"FM","Columbia marine","Channel 1081 / 81A US government environmental operations")
    )

    // Public monitoring directories; no receive tone filters so all analog
    // traffic on these frequencies remains audible. Legacy listings may be quiet.
    // https://www.police-frequencies.com/oregon/columbia/
    // https://status.broadcastify.com/listen/feed/30541
    // https://www.interceptradio.com/slocal.php?country=US&county=Columbia&disp=1&state=OR
    // https://www.interceptradio.com/slocal.php?city=Longview&country=US&county=Cowlitz&disp=1&state=WA
    val local = listOf(
        rx(40,"COLSHRF",154.785,"NFM","Local public safety","Columbia County law dispatch; listed PL 186.2; analog activity not confirmed"),
        rx(41,"COLFIRE",154.130,"NFM","Local public safety","Columbia County fire dispatch; listed PL 186.2"),
        rx(42,"COLMED2",154.355,"NFM","Local public safety","Columbia County secondary MEDNET/fire; listed PL 203.5"),
        rx(43,"COWFIRE",154.235,"NFM","Local public safety","Cowlitz County fire dispatch; analog activity not confirmed"),
        rx(44,"COWSHRF",154.815,"NFM","Local public safety","Cowlitz County sheriff dispatch; listed PL 156.7"),
        rx(45,"LVIEWPD",155.535,"NFM","Local public safety","Longview police dispatch; directory listing"),
        rx(46,"KELSOPD",156.090,"NFM","Local public safety","Kelso police dispatch; directory listing"),
        rx(47,"COWPUD",153.725,"NFM","Local utility","Cowlitz PUD historical scanner listing; current mode/activity unverified"),
        rx(48,"COLMED1",153.785,"NFM","Local public safety","Columbia County MEDNET; listed PL 186.2"),
        rx(49,"KALFIRE",153.920,"NFM","Local public safety","Kalama fire; directory listing")
    )

    // FCC-derived digital authorization does not establish current voice activity.
    // https://digitalfrequencysearch.com/Directory/OR/Columbia
    // Reference entries are intentionally not assigned default memory slots.
    val digitalReference = listOf(
        rx(0,"CRPUD1",152.960,"DMR","Local digital reference","Columbia River PUD WNAA640 Rainier; DMR authorized"),
        rx(0,"CRPUD2",160.695,"DMR","Local digital reference","Columbia River PUD WNAA640 Rainier; DMR authorized"),
        rx(0,"CLATPD",152.4275,"DMR","Local digital reference","Clatskanie PUD WQXV248; DMR authorized"),
        rx(0,"CLATWTR",153.620,"P25","Local digital reference","Clatskanie PUD WPTH881 Rainier; P25 authorized"),
        rx(0,"COLROAD",154.980,"P25","Local digital reference","Columbia County roads WQJB310; P25 authorized")
    )
    val defaults: List<Channel> get() = aviation + marine + local
}
