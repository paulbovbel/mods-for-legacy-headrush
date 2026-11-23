/* gui_core_cli.c -- milestone-1 CLI: built entirely on core/ (no debugfs/
 * e2fsck/xz/mkimage/python3 subprocess calls). Thin wrapper around
 * core/patch_pipeline.c -- the GUI app calls the exact same pipeline
 * function, see app/. Work-directory handling is portable (see
 * core/tempdir.h). Local dev/debugging tool only -- not built or shipped
 * by CI, see docs/BUILDING.md.
 *
 * usage: gui-core-cli <stock.img> <out.img> --model pedalboard|mx5|gigboard
 *                      [--anxiety-hijack [--instances 2|4]] [--no-nam-pedals] [--reverb-pedal] [--page-pedal [--page-down-note N] [--page-up-note N]]
 *                      [--keep-work-dir]
 *   or: gui-core-cli <input-directory> <plan-directory> --file-plan --model mx5 [--instances 2|4]
 *
 * Pedalboard default: adds the "NAM" and "NAM TWO" pedals (two instances each). --anxiety-hijack additionally (opt-in) makes Anxiety OD run
 * NAM (--instances 4 also hijacks Anxiety OD V2); --no-nam-pedals leaves out the new pedals (hijack only). MX5 and
 * Gigboard have no NAM pedals yet and always get the Anxiety OD hijack.
 */
#include "model_targets.h"
#include "patch_pipeline.h"
#include "tempdir.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

static void die(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "ERROR: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

static uint8_t* read_whole_file(const char* path, size_t* out_len)
{
  FILE* f = fopen(path, "rb");
  if (!f)
    die("opening %s: %s", path, strerror(errno));
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz < 0)
    die("ftell %s: %s", path, strerror(errno));
  fseek(f, 0, SEEK_SET);
  uint8_t* buf = (uint8_t*)malloc((size_t)sz ? (size_t)sz : 1);
  if (!buf)
    die("out of memory reading %s (%ld bytes)", path, sz);
  if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz)
    die("short read on %s", path);
  fclose(f);
  *out_len = (size_t)sz;
  return buf;
}

static void write_whole_file(const char* path, const uint8_t* data, size_t len)
{
  FILE* f = fopen(path, "wb");
  if (!f)
    die("opening %s for write: %s", path, strerror(errno));
  if (len > 0 && fwrite(data, 1, len, f) != len)
    die("short write on %s", path);
  if (fclose(f) != 0)
    die("closing %s: %s", path, strerror(errno));
}

static bool read_plan_input(const char* path, void* user_data, uint8_t** data, size_t* len,
                             char* err, size_t err_size)
{
  char local[4096];
  int n = snprintf(local, sizeof(local), "%s%s", (const char*)user_data, path);
  if (n < 0 || (size_t)n >= sizeof(local))
  {
    snprintf(err, err_size, "input path too long");
    return false;
  }
  *data = read_whole_file(local, len);
  return true;
}

static void write_plan(const char* directory, const NamFilePlan* plan)
{
  /* Refuse existing outputs. plan.json is written last as the completion marker. */
#ifdef _WIN32
  int result = _mkdir(directory);
#else
  int result = mkdir(directory, 0700);
#endif
  if (result != 0)
    die("creating plan directory %s: %s", directory, strerror(errno));
  char path[4096];
  for (size_t i = 0; i < plan->count; ++i)
  {
    int n = snprintf(path, sizeof(path), "%s/%zu.bin", directory, i);
    if (n < 0 || (size_t)n >= sizeof(path))
      die("output path too long");
    write_whole_file(path, plan->files[i].data, plan->files[i].len);
  }
  int n = snprintf(path, sizeof(path), "%s/plan.json", directory);
  if (n < 0 || (size_t)n >= sizeof(path))
    die("output path too long");
  FILE* manifest = fopen(path, "wb");
  if (!manifest)
    die("opening %s: %s", path, strerror(errno));
  fprintf(manifest, "{\"version\":1,\"files\":[\n");
  for (size_t i = 0; i < plan->count; ++i)
    fprintf(manifest, "%s{\"path\":\"%s\",\"file\":\"%zu.bin\",\"mode\":%u}",
            i ? ",\n" : "", plan->files[i].path, i, plan->files[i].mode);
  fprintf(manifest, "\n]}\n");
  bool failed = ferror(manifest) != 0;
  if (fclose(manifest) != 0 || failed)
    die("writing %s failed", path);
}

static void print_progress(const char* message, void* user_data)
{
  (void)user_data;
  printf("%s\n", message);
}

int main(int argc, char** argv)
{
  const char* usage =
      "usage: %s <stock.img> <out.img> --model pedalboard|mx5|gigboard [--anxiety-hijack [--instances 2|4]] "
      "[--no-nam-pedals] [--reverb-pedal] [--page-pedal [--page-down-note N] [--page-up-note N]] [--keep-work-dir]\n"
      "       with --file-plan: input is a directory mirroring device paths, output is a new plan directory";
  if (argc < 3)
    die(usage, argv[0]);

  const char* input_img = argv[1];
  const char* output_img = argv[2];
  const char* model_name = NULL;
  bool keep_work_dir = false;
  bool file_plan = false;
  int instances = 2;
  bool page_pedal = false;
  bool anxiety_hijack = false;
  bool no_nam_pedals = false;
  bool reverb_pedal = false;
  int page_down_note = -1, page_up_note = -1; /* -1 = the default */

  for (int i = 3; i < argc; ++i)
  {
    if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
      model_name = argv[++i];
    else if (strcmp(argv[i], "--instances") == 0 && i + 1 < argc)
    {
      const char* v = argv[++i];
      if (strcmp(v, "2") == 0)
        instances = 2;
      else if (strcmp(v, "4") == 0)
      {
        instances = 4;
        anxiety_hijack = true; /* the V2 hijack only exists on top of the Anxiety OD one */
      }
      else
        die("--instances must be 2 or 4, got %s", v);
    }
    else if (strcmp(argv[i], "--anxiety-hijack") == 0)
      anxiety_hijack = true;
    else if (strcmp(argv[i], "--no-nam-pedals") == 0)
      no_nam_pedals = true;
    else if (strcmp(argv[i], "--reverb-pedal") == 0)
      reverb_pedal = true;
    else if (strcmp(argv[i], "--page-pedal") == 0)
      page_pedal = true;
    else if (strcmp(argv[i], "--page-down-note") == 0 && i + 1 < argc)
      page_down_note = atoi(argv[++i]);
    else if (strcmp(argv[i], "--page-up-note") == 0 && i + 1 < argc)
      page_up_note = atoi(argv[++i]);
    else if (strcmp(argv[i], "--keep-work-dir") == 0)
      keep_work_dir = true;
    else if (strcmp(argv[i], "--file-plan") == 0)
      file_plan = true;
    else
      die("unknown argument: %s", argv[i]);
  }

  if (!model_name)
    die("--model is required (pedalboard|mx5|gigboard) -- no default target");

  /* only the Pedalboard has the NAM pedals; the other models keep the original (Anxiety OD) design */
  NamPatchOptions opts = {0};
  opts.nam_pedals = strcmp(model_name, "pedalboard") == 0 && !no_nam_pedals;
  opts.anxiety_hijack = anxiety_hijack || !opts.nam_pedals;
  opts.anxiety_v2 = instances == 4;
  opts.page_pedal = page_pedal;
  opts.reverb_pedal = reverb_pedal;
  if (reverb_pedal && !opts.nam_pedals)
    die("--reverb-pedal needs the NAM pedals (Pedalboard, without --no-nam-pedals)");
  if (page_down_note >= 0 || page_up_note >= 0)
  {
    if (!page_pedal)
      die("--page-down-note / --page-up-note need --page-pedal");
    opts.page_notes_set = true;
    opts.page_down_note = page_down_note >= 0 ? page_down_note : 10;
    opts.page_up_note = page_up_note >= 0 ? page_up_note : 4;
  }
  if (instances == 4 && !opts.anxiety_hijack)
    die("--instances 4 needs the Anxiety OD hijack");

  if (file_plan)
  {
    if (keep_work_dir)
      die("--keep-work-dir does not apply to --file-plan");
    NamFilePlan plan;
    char err[1024];
    if (!nam_patch_plan(read_plan_input, (void*)input_img, model_name, &opts,
                        print_progress, NULL, &plan, err, sizeof(err)))
      die("%s", err);
    write_plan(output_img, &plan);
    printf("OK  wrote %s/plan.json (%zu files; input files unchanged)\n", output_img, plan.count);
    nam_file_plan_free(&plan);
    return 0;
  }

  char workdir[NAM_TEMPDIR_MAX_PATH];
  char tempdir_err[256];
  if (!nam_make_temp_dir(workdir, sizeof(workdir), tempdir_err, sizeof(tempdir_err)))
    die("creating work directory: %s", tempdir_err);
  printf("working directory: %s\n", workdir);

  size_t input_len;
  uint8_t* input_data = read_whole_file(input_img, &input_len);

  /* the GUI refuses anything but the tested stock image; the CLI also takes dev bases (USB console builds), so it
   * only says which one it got */
  const ModelTarget* target = nam_select_target(model_name, NULL, NULL);
  char hex[65] = "?";
  if (target && nam_is_known_stock_img(target, input_data, input_len, hex))
    printf("OK  input is the tested stock %s 2.7 image (sha256 %.12s...)\n", model_name, hex);
  else
    printf("note: input is not the tested stock %s 2.7 image (sha256 %.12s...); the GUI would refuse it\n", model_name,
           hex);

  uint8_t* out_data;
  size_t out_len;
  char err[1024];
  bool ok = nam_patch_pipeline(input_data, input_len, model_name, &opts, workdir, print_progress, NULL, &out_data,
                                &out_len, err, sizeof(err));
  free(input_data);

  if (!ok)
  {
    if (!keep_work_dir)
      nam_remove_dir_recursive(workdir);
    die("%s", err);
  }

  write_whole_file(output_img, out_data, out_len);
  printf("OK  wrote %s (%zu bytes)\n", output_img, out_len);
  free(out_data);

  if (keep_work_dir)
    printf("--keep-work-dir: build directory left at %s\n", workdir);
  else
    nam_remove_dir_recursive(workdir);

  if (opts.nam_pedals)
    printf("\nOK  NAM pedals added (\"NAM\" and \"NAM TWO\" in the Distortion list, two instances each).\n");
  if (opts.anxiety_hijack)
    printf("%sOK  Anxiety OD (v1)%s process() NAM hijack applied.\n", opts.nam_pedals ? "" : "\n",
           opts.anxiety_v2 ? " + Anxiety OD V2" : "");
  if (page_pedal)
    printf("OK  Footswitch page-turner installed (FS5/FS11 -> USB Page Down/Up, see docs/PAGE_PEDAL.md).\n");
  return 0;
}
