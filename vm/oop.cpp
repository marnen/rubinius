#include "oop.hpp"
#include "builtin/object.hpp"

#include <cassert>

namespace rubinius {
  void ObjectHeader::initialize_copy(Object* other, unsigned int new_age) {
    /* Even though we dup it, we have to be careful to maintain
     * the zone. */

    obj_type     = other->obj_type;
    age          = new_age;
    field_count  = other->field_count;
    klass_       = other->klass_;
    ivars_       = other->ivars_;

    Forwarded    = 0;
    ForeverYoung = other->ForeverYoung;
    StoresBytes  = other->StoresBytes;
    RequiresCleanup = other->RequiresCleanup;
    IsBlockContext = other->IsBlockContext;
    IsMeta       = other->IsMeta;
    IsTainted    = other->IsTainted;
    IsFrozen     = other->IsFrozen;
    RefsAreWeak  = other->RefsAreWeak;
  }

  void ObjectHeader::copy_body(Object* other) {
    assert(this->field_count == other->field_count);

    void** src = other->__body__;
    void** dst = this->__body__;

    for(size_t counter = 0; counter < field_count; counter++) {
      dst[counter] = src[counter];
    }
  }

  /* Clear the body of the object, by setting each field to Qnil */
  void ObjectHeader::clear_fields() {
    ivars_ = Qnil;

    /* HACK: this case seems like a reasonable exception
     * to using accessor functions
     */
    void** dst = this->__body__;

    for(size_t counter = 0; counter < field_count; counter++) {
      dst[counter] = Qnil;
    }
  }

  void ObjectHeader::clear_body_to_null() {
    void** dst = this->__body__;

    for(size_t counter = 0; counter < field_count; counter++) {
      dst[counter] = 0;
    }

  }

}
