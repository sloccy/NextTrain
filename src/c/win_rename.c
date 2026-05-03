#include "win_rename.h"
#include "state.h"
#include <string.h>

static DictationSession *s_session = NULL;
static uint8_t           s_index;
static RenameCompleteCb  s_cb;
static char              s_buffer[64];

static void prv_handler(DictationSession *session, DictationSessionStatus status,
                        char *transcription, void *context) {
  if (status == DictationSessionStatusSuccess && transcription && transcription[0]) {
    char name[FAVORITE_NAME_LEN];
    strncpy(name, transcription, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    state_set_favorite_name(s_index, name);
  }
  dictation_session_destroy(session);
  s_session = NULL;
  if (s_cb) s_cb();
}

void win_rename_start(uint8_t favorite_index, RenameCompleteCb cb) {
  if (s_session) {
    dictation_session_destroy(s_session);
    s_session = NULL;
  }
  s_index = favorite_index;
  s_cb    = cb;
  s_session = dictation_session_create(sizeof(s_buffer), prv_handler, NULL);
  if (s_session) {
    dictation_session_start(s_session);
  } else if (cb) {
    cb(); // dictation unavailable — complete immediately with no change
  }
}
