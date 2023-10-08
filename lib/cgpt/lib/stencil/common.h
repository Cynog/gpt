/*
    GPT - Grid Python Toolkit
    Copyright (C) 2023  Christoph Lehner (christoph.lehner@ur.de, https://github.com/lehner/gpt)

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

// cartesian stencil fetch
#define fetch_cs(sidx, obj, point, site, view, do_adj) {		\
    if (point == -1) {							\
      obj = coalescedRead(view[site]);					\
    } else {								\
      int ptype;							\
      auto SE = sview[sidx].GetEntry(ptype, point, site);		\
      if (SE->_is_local) {						\
	obj = coalescedReadPermute(view[SE->_offset],ptype,SE->_permute); \
      } else {								\
	obj = coalescedRead(buf[sidx][SE->_offset]);			\
      }									\
      acceleratorSynchronise();						\
    }									\
    if (do_adj)								\
      obj = adj(obj);							\
  }

// general local stencil fetch
#ifndef GRID_HAS_ACCELERATOR

// cpu fetch version
#define fetch(obj, point, site, view, do_adj) {				\
    auto _SE = sview.GetEntry(point,site);				\
    obj = coalescedRead(view[_SE->_offset]);				\
    auto tmp = obj;							\
    if (_SE->_permute)							\
      for (int d=0;d<nd;d++)						\
	if (_SE->_permute & (0x1 << d)) { permute(obj,tmp,d); tmp=obj;}	\
    if (do_adj)								\
      obj = adj(obj);							\
  }

#else

template<class vobj> accelerator_inline
typename vobj::scalar_object coalescedReadGeneralPermute(const vobj & __restrict__ vec,int permute,int nd,int lane=acceleratorSIMTlane(vobj::Nsimd()))
{
  int plane = lane;
  for (int d=0;d<nd;d++)
    plane = (permute & (0x1 << d)) ? plane ^ (vobj::Nsimd() >> (d + 1)) : plane;
  return extractLane(plane,vec);
}

// gpu fetch version
#define fetch(obj, point, site, view, do_adj) {				\
    auto _SE = sview.GetEntry(point,site);				\
    if (_SE->_permute) {						\
      obj = coalescedReadGeneralPermute(view[_SE->_offset], _SE->_permute,nd); \
    } else {								\
      obj = coalescedRead(view[_SE->_offset]);				\
    }									\
    acceleratorSynchronise();						\
    if (do_adj)								\
      obj = adj(obj);							\
  }

// maybe also try only calling GeneralPermute for _permute == 0 case without sync

#endif

#include <Grid/stencil/GeneralLocalStencil.h>

// forward declarations
template<typename T>
void cgpt_basis_fill(PVector<Lattice<T>>& basis, const std::vector<cgpt_Lattice_base*>& _basis);

template<typename T>
bool is_compatible(cgpt_Lattice_base* other);

// traits to identify types for which type(a*a) = type(a)
template<typename T>     struct isEndomorphism                : public std::true_type { static constexpr bool notvalue = false; };
template<typename T>     struct isEndomorphism<iScalar<T>>    : public isEndomorphism<T> { static constexpr bool notvalue = isEndomorphism<T>::notvalue; };
template<typename T, int N>     struct isEndomorphism<iMatrix<T,N>>    : public isEndomorphism<T> { static constexpr bool notvalue = isEndomorphism<T>::notvalue; };
template<typename T, int N>     struct isEndomorphism<iVector<T,N>>    : public std::false_type { static constexpr bool notvalue = true; };

template<typename T, int N>    struct matrixFromTypeAtLevel { typedef T type; };
template<typename T, int N>    struct matrixFromTypeAtLevel<iScalar<T>,N> { typedef iScalar<typename matrixFromTypeAtLevel<T,N-1>::type> type; };
template<typename T, int N, int M>    struct matrixFromTypeAtLevel<iVector<T,M>,N> { typedef iScalar<typename matrixFromTypeAtLevel<T,N-1>::type> type; };
template<typename T, int N, int M>    struct matrixFromTypeAtLevel<iMatrix<T,M>,N> { typedef iScalar<typename matrixFromTypeAtLevel<T,N-1>::type> type; };

template<typename T>    struct matrixFromTypeAtLevel<iScalar<T>,0> { typedef iScalar<typename matrixFromTypeAtLevel<T,-1>::type> type; };
template<typename T, int M>    struct matrixFromTypeAtLevel<iVector<T,M>,0> { typedef iMatrix<typename matrixFromTypeAtLevel<T,-1>::type,M> type; };
template<typename T, int M>    struct matrixFromTypeAtLevel<iMatrix<T,M>,0> { typedef iMatrix<typename matrixFromTypeAtLevel<T,-1>::type,M> type; };

template<typename T, int N1, int N2>    struct matrixFromTypeAtLevel2;
template<typename T, int N1, int N2>    struct matrixFromTypeAtLevel2<iScalar<T>,N1,N2> { typedef iScalar<typename matrixFromTypeAtLevel2<T,N1-1,N2-1>::type> type; };
template<typename T, int N1, int N2, int M>    struct matrixFromTypeAtLevel2<iVector<T,M>,N1,N2> { typedef iScalar<typename matrixFromTypeAtLevel2<T,N1-1,N2-1>::type> type; };
template<typename T, int N1, int N2, int M>    struct matrixFromTypeAtLevel2<iMatrix<T,M>,N1,N2> { typedef iScalar<typename matrixFromTypeAtLevel2<T,N1-1,N2-1>::type> type; };

template<typename T, int N2>    struct matrixFromTypeAtLevel2<iScalar<T>,0,N2> { typedef iScalar<typename matrixFromTypeAtLevel<T,N2-1>::type> type; };
template<typename T, int N2, int M>    struct matrixFromTypeAtLevel2<iVector<T,M>,0,N2> { typedef iMatrix<typename matrixFromTypeAtLevel<T,N2-1>::type,M> type; };
template<typename T, int N2, int M>    struct matrixFromTypeAtLevel2<iMatrix<T,M>,0,N2> { typedef iMatrix<typename matrixFromTypeAtLevel<T,N2-1>::type,M> type; };

// cartesian
static void cgpt_shift_to_cartesian_dir_disp(const Coordinate& s, int& dir, int& disp) {
  dir = -1;
  for (int i=0;i<s.size();i++) {
    if (s[i] != 0) {
      if (dir != -1) {
	ERR("Only cartesian stencil allowed!  s[%d] != 0 and s[%d] != 0", dir, i);
      } else {
	dir = i;
	disp = s[i];
      }
    }
  }
}

template<typename CartesianStencil_t>
class CartesianStencilManager {
public:
  GridBase* grid;
  const std::vector<Coordinate>& shifts;
  std::map<int, std::set<int> > field_points;
  std::vector<CartesianStencil_t> stencils;
  Vector<int> stencil_map;
  std::map<int, std::map<int, int> > field_point_map;
	
  CartesianStencilManager(GridBase* _grid, const std::vector<Coordinate>& _shifts) : grid(_grid), shifts(_shifts) {
  }

  bool is_trivial(int point) {
    for (auto s : shifts[point])
      if (s != 0)
	return false;
    return true;
  }

  void register_point(int index, int point) {
    // register non-trivial points
    if (is_trivial(point))
      return;
	  
    field_points[index].insert(point);
	    
    if ((int)stencil_map.size() < index + 1)
      stencil_map.resize(index + 1, -1);
  }

  bool create_stencils(bool first_stencil) {
    // all fields in field_points now need a stencil
    for (auto & fp : field_points) {

      int index = fp.first;

      //std::cout << GridLogMessage << "Field " << index << " needs the following stencil:" << std::endl;

      std::vector<int> dirs, disps;
      for (auto p : fp.second) {
	int dir, disp;
	cgpt_shift_to_cartesian_dir_disp(shifts[p], dir, disp);
	ASSERT(dir != -1);

	field_point_map[index][p] = (int)dirs.size();
	      
	dirs.push_back(dir);
	disps.push_back(disp);

	//std::cout << GridLogMessage << " dir = " << dir << ", disp = " << disp << std::endl;
      }
      
      stencil_map[index] = (int)stencils.size();
      stencils.push_back(CartesianStencil_t(grid,dirs.size(),Even,dirs,disps,SimpleStencilParams(),!first_stencil));

      first_stencil = false;
    }

    return first_stencil;
  }

  int map_point(int index, int point) {
    if (is_trivial(point))
      return -1;
    return field_point_map[index][point];
  }
	
};
