#include <vector>
#include <string>
#include "libretro.h"
#include "burner.h"
#include "input/inp_keys.h"
#include "state.h"
#include <string.h>
#include <stdio.h>

#include <ctype.h>

#include "cd/cd_interface.h"

#define FBA_VERSION "v0.2.97.29" // Sept 16, 2013 (SVN)

#ifdef _WIN32
   char slash = '\\';
#else
   char slash = '/';
#endif

static retro_environment_t environ_cb;
static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_cb;
static retro_audio_sample_batch_t audio_batch_cb;

// FBARL ---

static unsigned int BurnDrvGetIndexByName(const char* name);

static bool g_opt_bUseUNIBIOS = false;

#define STAT_NOFIND	0
#define STAT_OK		1
#define STAT_CRC	   2
#define STAT_SMALL	3
#define STAT_LARGE	4

struct ROMFIND
{
	unsigned int nState;
	int nArchive;
	int nPos;
   BurnRomInfo ri;
};

static std::vector<std::string> g_find_list_path;
static ROMFIND g_find_list[1024];
static unsigned g_rom_count;

#define AUDIO_SAMPLERATE 32000
#define AUDIO_SEGMENT_LENGTH 534 // <-- Hardcoded value that corresponds well to 32kHz audio.

static uint16_t *g_fba_frame;
static int16_t g_audio_buf[AUDIO_SEGMENT_LENGTH * 2];

// libretro globals

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   static const struct retro_variable vars[] = {
      { "fba-diagnostics", "Diagnostics; disabled|enabled" },
      // Use UniBios by default. Original Bios takes a long time to load(about 2 minutes), no idea why.
      { "fba-unibios", "Neo Geo UniBIOS; enabled|disabled" },
      { "fba-cpu-speed-adjust", "CPU Speed Overclock; 100|110|120|130|140|150|160|170|180|190|200" },
      { NULL, NULL },
   };

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

char g_rom_dir[1024];
char g_save_dir[1024];
static bool driver_inited;

void retro_get_system_info(struct retro_system_info *info)
{
   info->library_name = "FB Alpha 2012 Neo Geo";
   info->library_version = FBA_VERSION;
   info->need_fullpath = true;
   info->block_extract = true;
   info->valid_extensions = "iso|zip";
}

/////
static void poll_input();
static bool init_input();

// FBA stubs
unsigned ArcadeJoystick;

int bDrvOkay;
int bRunPause;
BOOL bAlwaysProcessKeyboardInput;

BOOL bDoIpsPatch;
void IpsApplyPatches(UINT8 *, char *) {}

TCHAR szAppHiscorePath[MAX_PATH];
TCHAR szAppSamplesPath[MAX_PATH];
TCHAR szAppBurnVer[16];

CDEmuStatusValue CDEmuStatus;

const char* isowavLBAToMSF(const int LBA) { return ""; }
int isowavMSFToLBA(const char* address) { return 0; }
TCHAR* GetIsoPath() { return NULL; }
INT32 CDEmuInit() { return 0; }
INT32 CDEmuExit() { return 0; }
INT32 CDEmuStop() { return 0; }
INT32 CDEmuPlay(UINT8 M, UINT8 S, UINT8 F) { return 0; }
INT32 CDEmuLoadSector(INT32 LBA, char* pBuffer) { return 0; }
UINT8* CDEmuReadTOC(INT32 track) { return 0; }
UINT8* CDEmuReadQChannel() { return 0; }
INT32 CDEmuGetSoundBuffer(INT16* buffer, INT32 samples) { return 0; }

static unsigned char nPrevDIPSettings[4];
static int nDIPOffset;

static void InpDIPSWGetOffset (void)
{
	BurnDIPInfo bdi;
	nDIPOffset = 0;

	for(int i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
	{
		if (bdi.nFlags == 0xF0)
		{
			nDIPOffset = bdi.nInput;
         if (log_cb)
            log_cb(RETRO_LOG_INFO, "DIP switches offset: %d.\n", bdi.nInput);
			break;
		}
	}
}

void InpDIPSWResetDIPs (void)
{
	int i = 0;
	BurnDIPInfo bdi;
	struct GameInp * pgi = NULL;

	InpDIPSWGetOffset();

	while (BurnDrvGetDIPInfo(&bdi, i) == 0)
	{
		if (bdi.nFlags == 0xFF)
		{
			pgi = GameInp + bdi.nInput + nDIPOffset;
			if (pgi)
				pgi->Input.Constant.nConst = (pgi->Input.Constant.nConst & ~bdi.nMask) | (bdi.nSetting & bdi.nMask);	
		}
		i++;
	}
}

static int InpDIPSWInit()
{
   BurnDIPInfo bdi;
   struct GameInp *pgi;

   InpDIPSWGetOffset();
   InpDIPSWResetDIPs();

   // TODO: why does this crash on Wii?
#if 0
   for(int i = 0, j = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
   {
      /* 0xFE is the beginning label for a DIP switch entry */
      /* 0xFD are region DIP switches */
      if (bdi.nFlags == 0xFE || bdi.nFlags == 0xFD)
      {
         if (log_cb)
            log_cb(RETRO_LOG_INFO, "DIP switch label: %s.\n", bdi.szText);

         int l = 0;
         for (int k = 0; l < bdi.nSetting; k++)
         {
            BurnDIPInfo bdi_tmp;
            BurnDrvGetDIPInfo(&bdi_tmp, k+i+1);

            if (bdi_tmp.nMask == 0x3F ||
                  bdi_tmp.nMask == 0x30) /* filter away NULL entries */
               continue;

            if (log_cb)
               log_cb(RETRO_LOG_INFO, "DIP switch option: %s.\n", bdi_tmp.szText);
            l++;
         }
         pgi = GameInp + bdi.nInput + nDIPOffset;
         nPrevDIPSettings[j] = pgi->Input.Constant.nConst;
         j++;
      }
   }
#endif

   return 0;
}

int InputSetCooperativeLevel(const bool bExclusive, const bool bForeGround) { return 0; }

void Reinitialise(void) { }

// Non-idiomatic (OutString should be to the left to match strcpy())
// Seems broken to not check nOutSize.
char* TCHARToANSI(const TCHAR* pszInString, char* pszOutString, int /*nOutSize*/)
{
   if (pszOutString)
   {
      strcpy(pszOutString, pszInString);
      return pszOutString;
   }

   return (char*)pszInString;
}

int32_t QuoteRead(char **, char **, char*) { return 1; }
char *LabelCheck(char *, char *) { return 0; }
const int32_t nConfigMinVersion = 0x020921;

// addition to support loading of roms without crc check
static int find_rom_by_name(char *name, const ZipEntry *list, unsigned elems)
{
	unsigned i = 0;
	for (i = 0; i < elems; i++)
	{
		if(!strcmp(list[i].szName, name)) 
			return i; 
	}

#if 0
   if (log_cb)
      log_cb(RETRO_LOG_ERROR, "Not found: %s (name = %s)\n", list[i].szName, name);
#endif

	return -1;
}

static int find_rom_by_crc(uint32_t crc, const ZipEntry *list, unsigned elems)
{
	unsigned i = 0;
   for (i = 0; i < elems; i++)
   {
      if (list[i].nCrc == crc)
	  {
         return i;
	  }
   }

#if 0
   if (log_cb)
      log_cb(RETRO_LOG_ERROR, "Not found: 0x%X (crc: 0x%X)\n", list[i].nCrc, crc);
#endif
   
   return -1;
}

static void free_archive_list(ZipEntry *list, unsigned count)
{
   if (list)
   {
      for (unsigned i = 0; i < count; i++)
         free(list[i].szName);
      free(list);
   }
}

static int32_t archive_load_rom(uint8_t *dest, int32_t *wrote, int32_t i)
{
   if (i < 0 || i >= g_rom_count)
      return 1;

   int archive = g_find_list[i].nArchive;

   if (ZipOpen((char*)g_find_list_path[archive].c_str()) != 0)
      return 1;

   BurnRomInfo ri = {0};
   BurnDrvGetRomInfo(&ri, i);

   if (ZipLoadFile(dest, ri.nLen, wrote, g_find_list[i].nPos) != 0)
   {
      ZipClose();
      return 1;
   }

   ZipClose();
   return 0;
}
#ifdef GEKKO
/* Gets cache directory when using VM for large games. */
int get_cache_path(char *path)
{
	const char *system_directory_c = NULL;
	environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_directory_c);

	sprintf(path, "%s/cache/%s_cache/", system_directory_c, BurnDrvGetTextA(DRV_NAME));
}
#endif

// This code is very confusing. The original code is even more confusing :(
static bool open_archive(void)
{
	memset(g_find_list, 0, sizeof(g_find_list));

   struct retro_variable var = {0};
   var.key = "fba-unibios";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (!strcmp(var.value, "enabled"))
         g_opt_bUseUNIBIOS = true;
   }

	// FBA wants some roms ... Figure out how many.
	g_rom_count = 0;
	while (!BurnDrvGetRomInfo(&g_find_list[g_rom_count].ri, g_rom_count))
		g_rom_count++;

	g_find_list_path.clear();
	
	// Check if we have said archives.
	// Check if archives are found. These are relative to g_rom_dir.
	char *rom_name;
	for (unsigned index = 0; index < 32; index++)
	{
		if (BurnDrvGetZipName(&rom_name, index))
			continue;

      if (log_cb)
         log_cb(RETRO_LOG_INFO, "[FBA] Archive: %s\n", rom_name);

		char path[1024];
#ifdef _XBOX
		snprintf(path, sizeof(path), "%s\\%s", g_rom_dir, rom_name);
#else
		snprintf(path, sizeof(path), "%s/%s", g_rom_dir, rom_name);
#endif

		if (ZipOpen(path) != 0)
		{
         if (log_cb)
            log_cb(RETRO_LOG_ERROR, "[FBA] Failed to find archive: %s\n", path);
			return false;
		}
		ZipClose();

		g_find_list_path.push_back(path);
	}

	for (unsigned z = 0; z < g_find_list_path.size(); z++)
	{
		if (ZipOpen((char*)g_find_list_path[z].c_str()) != 0)
		{
         if (log_cb)
            log_cb(RETRO_LOG_ERROR, "[FBA] Failed to open archive %s\n", g_find_list_path[z].c_str());
			return false;
		}

		ZipEntry *list = NULL;
      int32_t count;
		ZipGetList(&list, &count);

		

		// Try to map the ROMs FBA wants to ROMs we find inside our pretty archives ...
		for (unsigned i = 0; i < g_rom_count; i++)
		{
			if (g_find_list[i].nState == STAT_OK)
				continue;

			if (g_find_list[i].ri.nType == 0 || g_find_list[i].ri.nLen == 0 || g_find_list[i].ri.nCrc == 0)
			{
				g_find_list[i].nState = STAT_OK;
				continue;
			}

			int index = -1;

			// USE UNI-BIOS...
			if (g_opt_bUseUNIBIOS) 
			{
				char *szPossibleName=NULL;
				BurnDrvGetRomName(&szPossibleName, i, 0);
				if(!strcmp(szPossibleName, "asia-s3.rom"))
				{
					if(index < 0) { index = find_rom_by_name((char*)"uni-bios_3_2.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0xA4E8B9B3, list, count); }
					if(index < 0) { index = find_rom_by_name((char*)"uni-bios_3_1.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x0C58093F, list, count); }
					if(index < 0) { index = find_rom_by_name((char*)"uni-bios_3_0.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0xA97C89A9, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_2_3o.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x601720AE, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_2_3.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x27664EB5, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_2_2.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x2D50996A, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_2_1.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x8DABF76B, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_2_0.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x0C12C2AD, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_1_3.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0xB24B44A0, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_1_2o.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0xE19D3CE9, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_1_2.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x4FA698E9, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_1_1.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x5DDA0D84, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_1_0.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x0CE453A0, list, count); }
					
					// uni-bios not found, try to find regular bios
					if(index < 0) {	index = find_rom_by_crc(g_find_list[i].ri.nCrc, list, count); }

				} else {
					index = find_rom_by_crc(g_find_list[i].ri.nCrc, list, count);
				}
			} else {
				index = find_rom_by_crc(g_find_list[i].ri.nCrc, list, count);
			}

			if (index < 0)
				continue;

			// Yay, we found it!
			g_find_list[i].nArchive = z;
			g_find_list[i].nPos = index;
			g_find_list[i].nState = STAT_OK;

			if (list[index].nLen < g_find_list[i].ri.nLen)
				g_find_list[i].nState = STAT_SMALL;
			else if (list[index].nLen > g_find_list[i].ri.nLen)
				g_find_list[i].nState = STAT_LARGE;
		}

		free_archive_list(list, count);
		ZipClose();
	}

	// Going over every rom to see if they are properly loaded before we continue ...
	for (unsigned i = 0; i < g_rom_count; i++)
	{
		if (g_find_list[i].nState != STAT_OK)
		{
         if (log_cb)
            log_cb(RETRO_LOG_ERROR, "[FBA] ROM index %i was not found ... CRC: 0x%08x\n", i, g_find_list[i].ri.nCrc);

			if(!(g_find_list[i].ri.nType & BRF_OPT)) {
				return false;
			}
		}
	}

	BurnExtLoadRom = archive_load_rom;
	return true;
}

void retro_init()
{
   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   g_fba_frame = (uint16_t*)malloc(320 * 224 * sizeof(uint16_t));
	BurnLibInit();
}

void retro_deinit()
{
   char output[128];

   if (driver_inited)
   {
      sprintf (output, "%s%c%s.fs", g_save_dir, slash, BurnDrvGetTextA(DRV_NAME));
      BurnStateSave(output, 0);
      BurnDrvExit();
   }
   driver_inited = false;
   BurnLibExit();
   if (g_fba_frame)
      free(g_fba_frame);
}

extern "C" {
   void HiscoreApply(void);
   void NeoFrame(void);
};

void retro_reset(void)
{
   struct GameInp* pgi = GameInp;

   for (unsigned i = 0; i < nGameInpCount; i++, pgi++)
   {
      if (pgi->Input.Switch.nCode != FBK_F3)
         continue;

      pgi->Input.nVal = 1;
      *(pgi->Input.pVal) = pgi->Input.nVal;

      break;
   }

   nBurnLayer = 0xff;
   pBurnSoundOut = g_audio_buf;
   nBurnSoundRate = AUDIO_SAMPLERATE;
   //nBurnSoundLen = AUDIO_SEGMENT_LENGTH;
   nCurrentFrame++;

   HiscoreApply();
   NeoFrame();
}

static bool first_init = true;

static void check_variables(void)
{
   struct retro_variable var = {0};
   var.key = "fba-diagnostics";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && !first_init)
   {
      static bool old_value = false;
      bool value = false;

      if (!strcmp(var.value, "disabled"))
         value = false;
      else if (!strcmp(var.value, "enabled"))
         value = true;

      if (old_value != value)
      {
         old_value = value;
         struct GameInp* pgi = GameInp;

         for (unsigned i = 0; i < nGameInpCount; i++, pgi++)
         {
            if (pgi->Input.Switch.nCode != FBK_F2)
               continue;

            pgi->Input.nVal = 1;
            *(pgi->Input.pVal) = pgi->Input.nVal;

            break;
         }

         nBurnLayer = 0xff;
         pBurnSoundOut = g_audio_buf;
         nBurnSoundRate = AUDIO_SAMPLERATE;
         //nBurnSoundLen = AUDIO_SEGMENT_LENGTH;
         nCurrentFrame++;

         HiscoreApply();
         NeoFrame();
      }
   }
   else if (first_init)
      first_init = false;

   var.key = "fba-unibios";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (!strcmp(var.value, "disabled"))
         g_opt_bUseUNIBIOS = false;
      if (!strcmp(var.value, "enabled"))
         g_opt_bUseUNIBIOS = true;
   }

   var.key = "fba-cpu-speed-adjust";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (!strcmp(var.value, "100"))
         nBurnCPUSpeedAdjust = 0x0100;
      else if (!strcmp(var.value, "110"))
         nBurnCPUSpeedAdjust = 0x0110;
      else if (strcmp(var.value, "120") == 0)
         nBurnCPUSpeedAdjust = 0x0120;
      else if (strcmp(var.value, "130") == 0)
         nBurnCPUSpeedAdjust = 0x0130;
      else if (strcmp(var.value, "140") == 0)
         nBurnCPUSpeedAdjust = 0x0140;
      else if (strcmp(var.value, "150") == 0)
         nBurnCPUSpeedAdjust = 0x0150;
      else if (strcmp(var.value, "160") == 0)
         nBurnCPUSpeedAdjust = 0x0160;
      else if (strcmp(var.value, "170") == 0)
         nBurnCPUSpeedAdjust = 0x0170;
      else if (strcmp(var.value, "180") == 0)
         nBurnCPUSpeedAdjust = 0x0180;
      else if (strcmp(var.value, "190") == 0)
         nBurnCPUSpeedAdjust = 0x0190;
      else if (strcmp(var.value, "200") == 0)
         nBurnCPUSpeedAdjust = 0x0200;
   }
}

void retro_run(void)
{
   int32_t width, height;
   BurnDrvGetFullSize(&width, &height);
   pBurnDraw = (uint8_t*)g_fba_frame;

   poll_input();

   nBurnLayer = 0xff;
   pBurnSoundOut = g_audio_buf;
   nBurnSoundRate = AUDIO_SAMPLERATE;
   //nBurnSoundLen = AUDIO_SEGMENT_LENGTH;


   unsigned drv_flags = BurnDrvGetFlags();
   size_t pitch_size = sizeof(uint16_t);
   nBurnPitch = width * pitch_size;

   nCurrentFrame++;
   HiscoreApply();
   NeoFrame();
   video_cb(g_fba_frame, width, height, nBurnPitch);
   audio_batch_cb(g_audio_buf, nBurnSoundLen);

   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables();
}

static uint8_t *write_state_ptr;
static const uint8_t *read_state_ptr;
static unsigned state_size;

static int32_t burn_write_state_cb(BurnArea *pba)
{
   memcpy(write_state_ptr, pba->Data, pba->nLen);
   write_state_ptr += pba->nLen;
   return 0;
}

static int32_t burn_read_state_cb(BurnArea *pba)
{
   memcpy(pba->Data, read_state_ptr, pba->nLen);
   read_state_ptr += pba->nLen;
   return 0;
}

static int32_t burn_dummy_state_cb(BurnArea *pba)
{
   state_size += pba->nLen;
   return 0;
}

size_t retro_serialize_size()
{
   if (state_size)
      return state_size;

   BurnAcb = burn_dummy_state_cb;
   state_size = 0;
   BurnAreaScan(ACB_FULLSCAN | ACB_READ, 0);
   return state_size;
}

bool retro_serialize(void *data, size_t size)
{
   if (size != state_size)
      return false;

   BurnAcb = burn_write_state_cb;
   write_state_ptr = (uint8_t*)data;
   BurnAreaScan(ACB_FULLSCAN | ACB_READ, 0);

   return true;
}

bool retro_unserialize(const void *data, size_t size)
{
   if (size != state_size)
      return false;
   BurnAcb = burn_read_state_cb;
   read_state_ptr = (const uint8_t*)data;
   BurnAreaScan(ACB_FULLSCAN | ACB_WRITE, 0);

   return true;
}

void retro_cheat_reset() {}
void retro_cheat_set(unsigned, bool, const char*) {}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   int32_t width, height;
   BurnDrvGetFullSize(&width, &height);
   int maximum = width > height ? width : height;
   struct retro_game_geometry geom = { width, height, maximum, maximum };

   struct retro_system_timing timing = { (nBurnFPS / 100.0), (nBurnFPS / 100.0) * AUDIO_SEGMENT_LENGTH };

   info->geometry = geom;
   info->timing   = timing;
}

int VidRecalcPal()
{
   return BurnRecalcPal();
}

static bool fba_init(unsigned driver, const char *game_zip_name)
{
   nBurnDrvActive = driver;

   if (!open_archive())
      return false;

   nBurnBpp = 2;
   nFMInterpolation = 3;
   nInterpolation = 3;
   
   char input[128];

   BurnDrvInit();
   sprintf (input, "%s%c%s.fs", g_save_dir, slash, BurnDrvGetTextA(DRV_NAME));
   BurnStateLoad(input, 0, NULL);

   int32_t width, height;
   BurnDrvGetFullSize(&width, &height);
   unsigned drv_flags = BurnDrvGetFlags();
   size_t pitch_size = nBurnBpp == 2 ? sizeof(uint16_t) : sizeof(uint32_t);
   nBurnPitch = width * pitch_size;

   unsigned rotation;
   switch (drv_flags & (BDF_ORIENTATION_FLIPPED | BDF_ORIENTATION_VERTICAL))
   {
      case BDF_ORIENTATION_VERTICAL:
         rotation = 1;
         break;

      case BDF_ORIENTATION_FLIPPED:
         rotation = 2;
         break;

      case BDF_ORIENTATION_VERTICAL | BDF_ORIENTATION_FLIPPED:
         rotation = 3;
         break;

      default:
         rotation = 0;
   }

   if(
         (!strcmp("gunbird2", game_zip_name)) ||
         (!strcmp("s1945ii", game_zip_name))  ||
         (!strcmp("s1945iii", game_zip_name)) ||
         (!strcmp("dragnblz", game_zip_name)) ||
         (!strcmp("gnbarich", game_zip_name)) ||
         (!strcmp("mjgtaste", game_zip_name)) ||
         (!strcmp("tgm2", game_zip_name))     ||
         (!strcmp("tgm2p", game_zip_name))    ||
         (!strcmp("soldivid", game_zip_name)) ||
         (!strcmp("daraku", game_zip_name))   ||
         (!strcmp("sbomber", game_zip_name))  ||
         (!strcmp("sbombera", game_zip_name)) 

         )
   {
      nBurnBpp = 4;
   }

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "Game: %s\n", game_zip_name);

   environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &rotation);

   VidRecalcPal();

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;

   if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt) && log_cb) 
      log_cb(RETRO_LOG_INFO, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");

   return true;
}

static void init_video()
{
}

static void extract_basename(char *buf, const char *path, size_t size)
{
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';

   char *ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char *base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
      buf[0] = '\0';
}

bool analog_controls_enabled = false;

bool retro_load_game(const struct retro_game_info *info)
{
   bool retval = false;
   char basename[128];

   if (!info)
      return false;

   extract_basename(basename, info->path, sizeof(basename));
   extract_directory(g_rom_dir, info->path, sizeof(g_rom_dir));

   const char *dir = NULL;
   // If save directory is defined use it, ...
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
   {
      strncpy(g_save_dir, dir, sizeof(g_save_dir));
      log_cb(RETRO_LOG_INFO, "Setting save dir to %s\n", g_save_dir);
   }
   else
   {
      // ... otherwise use rom directory
      strncpy(g_save_dir, g_rom_dir, sizeof(g_save_dir));
      log_cb(RETRO_LOG_ERROR, "Save dir not defined => use roms dir %s\n", g_save_dir);
   }

   unsigned i = BurnDrvGetIndexByName(basename);
   if (i < nBurnDrvCount)
   {
      pBurnSoundOut = g_audio_buf;
      nBurnSoundRate = AUDIO_SAMPLERATE;
      nBurnSoundLen = AUDIO_SEGMENT_LENGTH;

      if (!fba_init(i, basename))
         return false;

      driver_inited = true;
      analog_controls_enabled = init_input();

      retval = true;
   }
   else if (log_cb)
      log_cb(RETRO_LOG_ERROR, "[FBA] Cannot find driver.\n");

   InpDIPSWInit();

   check_variables();

   return retval;
}

bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

void retro_unload_game(void) {}

unsigned retro_get_region() { return RETRO_REGION_NTSC; }

void *retro_get_memory_data(unsigned) { return 0; }
size_t retro_get_memory_size(unsigned) { return 0; }

unsigned retro_api_version() { return RETRO_API_VERSION; }

void retro_set_controller_port_device(unsigned, unsigned) {}

// Input stuff.

// Ref GamcPlayer() in ../gamc.cpp
struct key_map
{
   const char *bii_name;
   unsigned nCode[2];
};
static uint8_t keybinds[0x5000][2]; 

#define BIND_MAP_COUNT 300

#define RETRO_DEVICE_ID_JOYPAD_RESET      16
#define RETRO_DEVICE_ID_JOYPAD_SERVICE    17
#define RETRO_DEVICE_ID_JOYPAD_DIAGNOSTIC 18
#define RETRO_DEVICE_ID_JOYPAD_DIP_A      19
#define RETRO_DEVICE_ID_JOYPAD_DIP_B      20
#define RETRO_DEVICE_ID_JOYPAD_TEST       21

static const char *print_label(unsigned i)
{
   switch(i)
   {
      case RETRO_DEVICE_ID_JOYPAD_B:
         return "RetroPad Button B";
      case RETRO_DEVICE_ID_JOYPAD_Y:
         return "RetroPad Button Y";
      case RETRO_DEVICE_ID_JOYPAD_SELECT:
         return "RetroPad Button Select";
      case RETRO_DEVICE_ID_JOYPAD_START:
         return "RetroPad Button Start";
      case RETRO_DEVICE_ID_JOYPAD_UP:
         return "RetroPad D-Pad Up";
      case RETRO_DEVICE_ID_JOYPAD_DOWN:
         return "RetroPad D-Pad Down";
      case RETRO_DEVICE_ID_JOYPAD_LEFT:
         return "RetroPad D-Pad Left";
      case RETRO_DEVICE_ID_JOYPAD_RIGHT:
         return "RetroPad D-Pad Right";
      case RETRO_DEVICE_ID_JOYPAD_A:
         return "RetroPad Button A";
      case RETRO_DEVICE_ID_JOYPAD_X:
         return "RetroPad Button X";
      case RETRO_DEVICE_ID_JOYPAD_L:
         return "RetroPad Button L";
      case RETRO_DEVICE_ID_JOYPAD_R:
         return "RetroPad Button R";
      case RETRO_DEVICE_ID_JOYPAD_L2:
         return "RetroPad Button L2";
      case RETRO_DEVICE_ID_JOYPAD_R2:
         return "RetroPad Button R2";
      case RETRO_DEVICE_ID_JOYPAD_L3:
         return "RetroPad Button L3";
      case RETRO_DEVICE_ID_JOYPAD_R3:
         return "RetroPad Button R3";
      default:
         return "No known label";
   }
}

#define PTR_INCR ((incr++ % 3 == 2) ? counter++ : counter)

static bool init_input(void)
{
   GameInpInit();
   GameInpDefault();

   bool has_analog = false;
   struct GameInp* pgi = GameInp;
   for (unsigned i = 0; i < nGameInpCount; i++, pgi++)
   {
      if (pgi->nType == BIT_ANALOG_REL)
      {
         has_analog = true;
         break;
      }
   }

   //needed for Neo Geo button mappings (and other drivers in future)
   const char * parentrom	= BurnDrvGetTextA(DRV_PARENT);
   const char * boardrom	= BurnDrvGetTextA(DRV_BOARDROM);
   const char * drvname		= BurnDrvGetTextA(DRV_NAME);
   INT32	genre		= BurnDrvGetGenreFlags();
   INT32	hardware	= BurnDrvGetHardwareCode();

   if (log_cb)
   {
      log_cb(RETRO_LOG_INFO, "has_analog: %d\n", has_analog);
      if(parentrom)
         log_cb(RETRO_LOG_INFO, "parentrom: %s\n", parentrom);
      if(boardrom)
         log_cb(RETRO_LOG_INFO, "boardrom: %s\n", boardrom);
      if(drvname)
         log_cb(RETRO_LOG_INFO, "drvname: %s\n", drvname);
      log_cb(RETRO_LOG_INFO, "genre: %d\n", genre);
      log_cb(RETRO_LOG_INFO, "hardware: %d\n", hardware);
   }

   /* initialization */
   struct BurnInputInfo bii;
   memset(&bii, 0, sizeof(bii));

   // Bind to nothing.
   for (unsigned i = 0; i < 0x5000; i++)
      keybinds[i][0] = 0xff;

   pgi = GameInp;

   key_map bind_map[BIND_MAP_COUNT];
   unsigned counter = 0;
   unsigned incr = 0;

   bind_map[PTR_INCR].bii_name = "P1 Coin";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Start";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Start 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Up";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Down";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Left";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Right";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Attack";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Accelerate";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Brake";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Gear";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* for Forgotten Worlds, etc */
   bind_map[PTR_INCR].bii_name = "P1 Turn";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Jump";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Pin";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Select";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Use";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Weak Punch";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Medium Punch";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Strong Punch";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Weak Kick";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Medium Kick";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Strong Kick";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Rotate Left";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Rotate Right";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Punch";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Kick";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Special";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Shot";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Shot (auto)";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* Simpsons - Konami */
   bind_map[PTR_INCR].bii_name = "P1 Button 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* Simpsons - Konami */
   bind_map[PTR_INCR].bii_name = "P1 Button 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Button 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Button 4";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* Progear */
   bind_map[PTR_INCR].bii_name = "P1 Auto";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* Punisher */
   bind_map[PTR_INCR].bii_name = "P1 Super";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Answer 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Answer 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Answer 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Answer 4";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Shot 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* Pang 3 */
   bind_map[PTR_INCR].bii_name = "P1 Shot 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* Pang 3 */
   bind_map[PTR_INCR].bii_name = "P1 Shot 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Bomb";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Special";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* for Ghouls 'n Ghosts */
   bind_map[PTR_INCR].bii_name = "P1 Fire";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* TMNT */
   bind_map[PTR_INCR].bii_name = "P1 Fire 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* Space Harrier */
   bind_map[PTR_INCR].bii_name = "Fire 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* Space Harrier */
   bind_map[PTR_INCR].bii_name = "Fire 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* Space Harrier */
   bind_map[PTR_INCR].bii_name = "Fire 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* TMNT */
   bind_map[PTR_INCR].bii_name = "P1 Fire 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* Strider */
   bind_map[PTR_INCR].bii_name = "P1 Fire 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* Strider */
   bind_map[PTR_INCR].bii_name = "Coin 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* Neo Geo */
   bind_map[PTR_INCR].bii_name = "P1 Button A";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* Neo Geo */
   bind_map[PTR_INCR].bii_name = "P1 Button B";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* Neo Geo */
   bind_map[PTR_INCR].bii_name = "P1 Button C";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* Neo Geo */
   bind_map[PTR_INCR].bii_name = "P1 Button D";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Coin";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Start";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Up";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Down";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Left";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Right";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Attack";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   // for Forgotten Worlds, etc.
   bind_map[PTR_INCR].bii_name = "P2 Turn";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Jump";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Pin";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Select";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Use";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Weak Punch";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Medium Punch";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Strong Punch";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Weak Kick";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Medium Kick";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Strong Kick";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Rotate Left";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Rotate Right";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Punch";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Kick";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Special";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Shot";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 1;

   /* Simpsons - Konami */
   bind_map[PTR_INCR].bii_name = "P2 Button 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Button 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 1;

   /* Various */
   bind_map[PTR_INCR].bii_name = "P2 Button 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Button 4";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 1;

   /* Progear */
   bind_map[PTR_INCR].bii_name = "P2 Auto";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Shot (auto)";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 1;

   /* Punisher */
   bind_map[PTR_INCR].bii_name = "P2 Super";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Answer 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Answer 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Answer 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Answer 4";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   /* Pang 3 */
   bind_map[PTR_INCR].bii_name = "P2 Shot 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Shot 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Bomb";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Special";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   /* Ghouls 'n Ghosts */
   bind_map[PTR_INCR].bii_name = "P2 Fire";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   /* TMNT */
   bind_map[PTR_INCR].bii_name = "P2 Fire 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Fire 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 1;

   /* Strider */
   bind_map[PTR_INCR].bii_name = "P2 Fire 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "Coin 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 1;

   /* Neo Geo */
   bind_map[PTR_INCR].bii_name = "P2 Button A";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Button B";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Button C";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Button D";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P3 Coin";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Start";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Up";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Down";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Left";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Right";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Attack";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Jump";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Pin";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Select";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Use";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 2;

   /* Simpsons - Konami */
   bind_map[PTR_INCR].bii_name = "P3 Button 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Button 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Button 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Button 4";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 2;

   /* TMNT */
   bind_map[PTR_INCR].bii_name = "P3 Fire 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Fire 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 2;

   /* Strider */
   bind_map[PTR_INCR].bii_name = "P3 Fire 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "Coin 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P4 Coin";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Start";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Up";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Down";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Left";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Right";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Attack";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Jump";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 3;
   
   bind_map[PTR_INCR].bii_name = "P4 Pin";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Select";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Use";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 3;

   /* Simpsons */
   bind_map[PTR_INCR].bii_name = "P4 Button 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Button 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Button 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Button 4";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 3;

   /* TMNT */
   bind_map[PTR_INCR].bii_name = "P4 Fire 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Fire 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Fire 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "Coin 4";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 3;

   /* Afterburner */
   bind_map[PTR_INCR].bii_name = "Missile";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Vulcan";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Throttle";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Up";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Down";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Left";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Right";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Start 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P1 Low Attack";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 High Attack";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Weapon";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Boost";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Low Attack";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 High Attack";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Weapon";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Boost";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   /* Bad Dudes */

   bind_map[PTR_INCR].bii_name = "Fire 4";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Fire 5";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Up (Cocktail)";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "Down (Cocktail)";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "Left (Cocktail)";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "Right (Cocktail)";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "Fire 1 (Cocktail)";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "Fire 2 (Cocktail)";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "Fire 3 (Cocktail)";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "Fire 4 (Cocktail)";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "Fire 5 (Cocktail)";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
   bind_map[PTR_INCR].nCode[1] = 1;

   /* Armed Police Batrider */
   bind_map[PTR_INCR].bii_name = "P1 Shoot 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Shoot 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Shoot 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Shoot 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Shoot 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Shoot 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 1;

   /* Chase HQ */

   bind_map[PTR_INCR].bii_name = "Turbo";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   /* WWF Wrestlefest */

   bind_map[PTR_INCR].bii_name = "Start 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "Start 4";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P1 Turn 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Turn 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P1 Turn 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Turn 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P1 start";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 start";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "Brake 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
   bind_map[PTR_INCR].nCode[1] = 0;
   
   bind_map[PTR_INCR].bii_name = "Brake 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L2;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Accelerate 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Accelerate 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Shot1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Shot2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Shot1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Shot2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P1 Attack Left";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Attack Right";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Attack Left";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Attack Right";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P1 Button 5";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Button 6";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Button 5";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Button 6";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
   bind_map[PTR_INCR].nCode[1] = 1;

   for(unsigned int i = 0; i < nGameInpCount; i++, pgi++)
   {
      BurnDrvGetInputInfo(&bii, i);

      bool value_found = false;

      for(int j = 0; j < counter; j++)
      {
         if(!strcmp(bii.szName,"P1 Select") && (boardrom && !strcmp(boardrom,"neogeo")))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
            keybinds[pgi->Input.Switch.nCode][1] = 0;
            value_found = true;
         }
         else if(!strcmp(bii.szName,"P1 Shot") && ((parentrom && !strcmp(parentrom,"avsp")) || !strcmp(drvname,"avsp")))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_A;
            keybinds[pgi->Input.Switch.nCode][1] = 0;
            value_found = true;
         }
         else if(!strcmp(bii.szName,"P2 Select") && (boardrom && !strcmp(boardrom,"neogeo")))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
            keybinds[pgi->Input.Switch.nCode][1] = 1;
            value_found = true;
         }
         else if(((parentrom && !strcmp(parentrom,"avsp")) || !strcmp(drvname,"avsp")) && (!strcmp(bii.szName,"P2 Shot")))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_A;
            keybinds[pgi->Input.Switch.nCode][1] = 1;
            value_found = true;
         }
         else if(!strcmp(bii.szName, bind_map[j].bii_name))
         {
            keybinds[pgi->Input.Switch.nCode][0] = bind_map[j].nCode[0];
            keybinds[pgi->Input.Switch.nCode][1] = bind_map[j].nCode[1];
            value_found = true;
         }
         else
            value_found = false;

         if (!value_found)
            continue;

         if (log_cb)
         {
            log_cb(RETRO_LOG_INFO, "%s - assigned to key: %s, port: %d.\n", bii.szName, print_label(keybinds[pgi->Input.Switch.nCode][0]),keybinds[pgi->Input.Switch.nCode][1]);
            log_cb(RETRO_LOG_INFO, "%s - has nSwitch.nCode: %x.\n", bii.szName, pgi->Input.Switch.nCode);
         }
         break;
      }

      if(!value_found && log_cb)
      {
         log_cb(RETRO_LOG_INFO, "WARNING! Button unaccounted for: [%s].\n", bii.szName);
         log_cb(RETRO_LOG_INFO, "%s - has nSwitch.nCode: %x.\n", bii.szName, pgi->Input.Switch.nCode);
      }
   }

   return has_analog;
}

//#define DEBUG_INPUT
//

static inline int CinpJoyAxis(int i, int axis)
{
   switch(axis)
   {
      case 0:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_X);
      case 1:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_Y);
      case 2:
         return 0;
      case 3:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_X);
      case 4:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_Y);
      case 5:
         return 0;
      case 6:
         return 0;
      case 7:
         return 0;
   }
   return 0;
}

static inline int CinpMouseAxis(int i, int axis)
{
   return 0;
}

static void poll_input(void)
{
   poll_cb();

   struct GameInp* pgi = GameInp;

   for (int i = 0; i < nGameInpCount; i++, pgi++)
   {
      int nAdd = 0;

      if ((pgi->nInput & GIT_GROUP_SLIDER) == 0)                           // not a slider
         continue;

      if (pgi->nInput == GIT_KEYSLIDER)
      {
         // Get states of the two keys
			if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
				nAdd -= 0x100;
			if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
				nAdd += 0x100;
      }

      // nAdd is now -0x100 to +0x100

      // Change to slider speed
      nAdd *= pgi->Input.Slider.nSliderSpeed;
      nAdd /= 0x100;

      if (pgi->Input.Slider.nSliderCenter)
      {                                          // Attact to center
         int v = pgi->Input.Slider.nSliderValue - 0x8000;
         v *= (pgi->Input.Slider.nSliderCenter - 1);
         v /= pgi->Input.Slider.nSliderCenter;
         v += 0x8000;
         pgi->Input.Slider.nSliderValue = v;
      }

      pgi->Input.Slider.nSliderValue += nAdd;
      // Limit slider
      if (pgi->Input.Slider.nSliderValue < 0x0100)
         pgi->Input.Slider.nSliderValue = 0x0100;
      if (pgi->Input.Slider.nSliderValue > 0xFF00)
         pgi->Input.Slider.nSliderValue = 0xFF00;
   }

   pgi = GameInp;

   for (unsigned i = 0; i < nGameInpCount; i++, pgi++)
   {
      switch (pgi->nInput)
      {
         case GIT_CONSTANT: // Constant value
            {
               pgi->Input.nVal = pgi->Input.Constant.nConst;
               *(pgi->Input.pVal) = pgi->Input.nVal;
            }
            break;
         case GIT_SWITCH:
            {
               // Digital input
               INT32 id = keybinds[pgi->Input.Switch.nCode][0];
               unsigned port = keybinds[pgi->Input.Switch.nCode][1];

               bool state = input_cb(port, RETRO_DEVICE_JOYPAD, 0, id);

#if 0
               log_cb(RETRO_LOG_INFO, "GIT_SWITCH: %s, port: %d, pressed: %d.\n", print_label(id), port, state);
#endif

               if (pgi->nType & BIT_GROUP_ANALOG)
               {
                  // Set analog controls to full
                  if (state)
                     pgi->Input.nVal = 0xFFFF;
                  else
                     pgi->Input.nVal = 0x0001;
#ifdef MSB_FIRST
                  *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
                  *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               }
               else
               {
                  // Binary controls
                  if (state)
                     pgi->Input.nVal = 1;
                  else
                     pgi->Input.nVal = 0;
                  *(pgi->Input.pVal) = pgi->Input.nVal;
               }
               break;
            }
         case GIT_KEYSLIDER:						// Keyboard slider
#if 0
            if (log_cb)
               log_cb(RETRO_LOG_INFO, "GIT_JOYSLIDER\n");
#endif
            {
               int nSlider = pgi->Input.Slider.nSliderValue;
               if (pgi->nType == BIT_ANALOG_REL) {
                  nSlider -= 0x8000;
                  nSlider >>= 4;
               }

               pgi->Input.nVal = (unsigned short)nSlider;
#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
         case GIT_MOUSEAXIS:						// Mouse axis
            {
               pgi->Input.nVal = (UINT16)(CinpMouseAxis(pgi->Input.MouseAxis.nMouse, pgi->Input.MouseAxis.nAxis) * nAnalogSpeed);
#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
            }
            break;
         case GIT_JOYAXIS_FULL:
            {				// Joystick axis
               INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);

               if (pgi->nType == BIT_ANALOG_REL) {
                  nJoy *= nAnalogSpeed;
                  nJoy >>= 13;

                  // Clip axis to 8 bits
                  if (nJoy < -32768) {
                     nJoy = -32768;
                  }
                  if (nJoy >  32767) {
                     nJoy =  32767;
                  }
               } else {
                  nJoy >>= 1;
                  nJoy += 0x8000;

                  // Clip axis to 16 bits
                  if (nJoy < 0x0001) {
                     nJoy = 0x0001;
                  }
                  if (nJoy > 0xFFFF) {
                     nJoy = 0xFFFF;
                  }
               }

               pgi->Input.nVal = (UINT16)nJoy;
#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
         case GIT_JOYAXIS_NEG:
            {				// Joystick axis Lo
               INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);
               if (nJoy < 32767)
               {
                  nJoy = -nJoy;

                  if (nJoy < 0x0000)
                     nJoy = 0x0000;
                  if (nJoy > 0xFFFF)
                     nJoy = 0xFFFF;

                  pgi->Input.nVal = (UINT16)nJoy;
               }
               else
                  pgi->Input.nVal = 0;

#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
         case GIT_JOYAXIS_POS:
            {				// Joystick axis Hi
               INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);
               if (nJoy > 32767)
               {

                  if (nJoy < 0x0000)
                     nJoy = 0x0000;
                  if (nJoy > 0xFFFF)
                     nJoy = 0xFFFF;

                  pgi->Input.nVal = (UINT16)nJoy;
               }
               else
                  pgi->Input.nVal = 0;

#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
      }
   }
}

static unsigned int BurnDrvGetIndexByName(const char* name)
{
   unsigned int i;
   unsigned int ret = ~0U;
   for (i = 0; i < nBurnDrvCount; i++)
   {
      nBurnDrvActive = i;
      if (!strcmp(BurnDrvGetText(DRV_NAME), name))
      {
         ret = i;
         break;
      }
   }
   return ret;
}

#ifdef ANDROID
#include <wchar.h>

size_t mbstowcs(wchar_t *pwcs, const char *s, size_t n)
{
   if (pwcs == NULL)
      return strlen(s);
   return mbsrtowcs(pwcs, &s, n, NULL);
}

size_t wcstombs(char *s, const wchar_t *pwcs, size_t n)
{
   return wcsrtombs(s, &pwcs, n, NULL);
}

#endif

}
