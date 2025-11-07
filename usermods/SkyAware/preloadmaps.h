#pragma once
#include <pgmspace.h>

namespace SkyAwarePreloads {
  struct CsvMap { uint8_t segment; const char* csv; };
  struct Preset { const char* name; const CsvMap* rows; uint8_t count; };

  static const char P_PNW_30x24_V1_SEG0[] PROGMEM = "KLKV,KAAT,KLMT,KSIY,KACV,KMFR,KRBG,KOTH,KEUG,KS21,KRDM,KS12,KCVO,KONP,KSLE,KS33,KDLS,KUAO,KMMV,KTMK,KHIO,KPDX,KSPB,KKLS,KAST,KHQM,KCLS,KOLM,KSHN,KTCM,KYKM,KELN,KEAT,KSEA,KUIL,KCLM,-,KPAE,KAWO,KNUW,CYYJ,KBVS,KBLI,KS52,KOMK,KDEW,KCOE,KSFF,KSKA,KMWH,KPSC,KHRI,KPDT,KBKE,KGCD,KBNO,KREO,K10U,KTWF,KGNG,KSUN,KMUO,KEUL,KDNJ,KGIC,KHRF,KMSO,KMLP,KGPI,KCTB,KGTF,KHLN,KBTM,KBZN,KRVF,KWYS,KDIJ,KIDA,K46U,KAFO,KPIH,K1U7,KLGU,KBMC,KBYI";

  static const CsvMap PNW_30x24_V1_ROWS[] PROGMEM = {
    { /*segment*/ 0, /*csv*/ P_PNW_30x24_V1_SEG0 },
  };

  static const Preset PRESETS[] PROGMEM = {
    { "PNW_30x24_V1", PNW_30x24_V1_ROWS, (uint8_t)(sizeof(PNW_30x24_V1_ROWS)/sizeof(PNW_30x24_V1_ROWS[0])) }
  };
  static constexpr uint8_t PRESET_COUNT = sizeof(PRESETS)/sizeof(PRESETS[0]);

} // namespace SkyAwarePreloads