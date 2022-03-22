#include <errno.h>
#include <glob.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* All temp values from sysfs are are in milldegree celsius */
#define C_TO_MILLIC(n) (n * 1000)
#define MILLIC_TO_C(n) (n / 1000)

#define TEMP_FILES_GLOB "/sys/class/thermal/thermal_zone*/temp"
#define FAN_CONTROL_FILE "/proc/acpi/ibm/fan"
#define TEMP_MAX_MCEL C_TO_MILLIC(150)

static const char *prog_name = NULL;

enum FanLevel {
    FAN_OFF = 0,
    FAN_LOW = 2,
    FAN_MED = 4,
    FAN_MAX = 7,
    FAN_UNKNOWN,
};

struct Rule {
    unsigned int threshold;
    enum FanLevel fan_level;
};

/* Must be ordered from highest to lowest temp */
static const struct Rule rules[] = {
    {80, FAN_MAX},
    {75, FAN_MED},
    {70, FAN_LOW},
    {0, FAN_OFF},
};

/*
 * How many degrees lower than the fan level activation speed you must be
 * before you're allowed to switch to a lower fan speed
 */
static unsigned int fan_hysteresis = 10;

static const struct Rule *current_rule = NULL;

static glob_t temp_files;
static int temp_files_populated = 0;

static int glob_err_handler(const char *epath, int eerrno) {
    fprintf(stderr, "glob: %s: %s\n", epath, strerror(eerrno));
    return 0;
}

static int64_t read_temp_file(const char *filename) {
    FILE *f;
    int ret;
    int64_t val;

    f = fopen(filename, "re");
    if (!f) {
        fprintf(stderr, "%s: fopen: %s\n", filename, strerror(errno));
        return errno;
    }

    ret = fscanf(f, "%" PRIi64, &val);
    if (!ret) {
        fprintf(stderr, "%s: fscanf: %s\n", filename, strerror(errno));
        return errno;
    }

    fclose(f);

    if (val <= 0 || val > TEMP_MAX_MCEL) {
        fprintf(stderr, "%s: invalid temperature: %" PRIi64 "\n", filename,
                val);
        return -ERANGE;
    }

    return val;
}

static int get_max_temp(void) {
    int64_t max_temp = 0;
    int ret;
    size_t i;

    if (!temp_files_populated) {
        ret = glob(TEMP_FILES_GLOB, 0, glob_err_handler, &temp_files);
        if (ret != 0) {
            const char *err = "glob: Unknown error";

            switch (ret) {
                case GLOB_NOMATCH:
                    err = "Could not find temperature file";
                    break;
                case GLOB_NOSPACE:
                    err = "glob: Out of memory";
                    break;
                default:
                    /*
                     * GLOB_ABORTED can't happen because we don't use GLOB_ERR
                     * and glob_err_handler always returns success
                     */
                    break;
            }
            fprintf(stderr, "%s: %s\n", prog_name, err);
        }
        temp_files_populated = 1;
    }

    for (i = 0; i < temp_files.gl_pathc; i++) {
        const char *tf = temp_files.gl_pathv[i];
        int64_t temp = read_temp_file(tf);

        if (temp > max_temp) {
            max_temp = temp;
        }
    }

    if (max_temp == 0) {
        fprintf(stderr, "Couldn't find any valid temperature\n");
        return -ENODATA;
    }

    return MILLIC_TO_C(max_temp);
}

static int _set_fan_level(enum FanLevel level) {
    FILE *f;
    int ret;

    f = fopen(FAN_CONTROL_FILE, "we");
    if (!f) {
        fprintf(stderr, "%s: fopen: %s\n", FAN_CONTROL_FILE, strerror(errno));
        return errno;
    }

    ret = fprintf(f, "level %d", level);
    if (!ret) {
        fprintf(stderr, "%s: fprintf: %s\n", FAN_CONTROL_FILE, strerror(errno));
        return errno;
    }

    fclose(f);

    printf("Set level %d\n", level);

    return 0;
}

static void set_fan_level(void) {
    unsigned int max_temp = (unsigned int)get_max_temp();
    unsigned int penalty = 0;
    size_t i;

    for (i = 0; i < (sizeof(rules) / sizeof(rules[0])); i++) {
        const struct Rule *rule = rules + i;

        if (rule == current_rule) {
            /* Penalty for all further rules for hysteresis */
            penalty = fan_hysteresis;
        }

        if (rule->threshold < penalty ||
            (rule->threshold - penalty) < max_temp) {
            if (rule != current_rule) {
                current_rule = rule;
                _set_fan_level(rule->fan_level);
            }
            return;
        }
    }

    fprintf(stderr, "No rule matched?\n");
}

int main(int argc, char *argv[]) {
    prog_name = argv[0];
    while (1) {
        set_fan_level();
        sleep(1);
    }

    /* TODO: reset to auto on exit */
    if (temp_files_populated) {
        globfree(&temp_files);
    }
}
