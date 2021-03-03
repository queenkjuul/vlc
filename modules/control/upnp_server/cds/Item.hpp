#pragma once

#include "../Common.hpp"

#include "Object.hpp"

namespace cds
{
class Item : public Object
{
    vlc_medialibrary_t& m_medialib;

    public:
    Item( const int64_t id, vlc_medialibrary_t& ) noexcept;
    void dump_metadata( xml::Element&, const Object::ExtraId&, const ClientProfile& ) const final;

    static void dump_mlobject_metadata( xml::Element& dest,
                                        const vlc_ml_media_t& media,
                                        vlc_medialibrary_t& ml,
                                        const ClientProfile& ) noexcept;
};
} // namespace cds
