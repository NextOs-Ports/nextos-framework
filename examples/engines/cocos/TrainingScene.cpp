// SPDX-License-Identifier: GPL-3.0-only
#include "TrainingScene.h"
#include <algorithm>
USING_NS_CC;
bool TrainingScene::init() {
    if(!Scene::init())return false;
    drawing=DrawNode::create();addChild(drawing);
    score=UserDefault::getInstance()->getIntegerForKey("nextos-training-score",0);
    auto keyboard=EventListenerKeyboard::create();
    auto key=[this](EventKeyboard::KeyCode code,bool held) {
        if(code==EventKeyboard::KeyCode::KEY_LEFT_ARROW)left=held;
        if(code==EventKeyboard::KeyCode::KEY_RIGHT_ARROW)right=held;
        if(code==EventKeyboard::KeyCode::KEY_UP_ARROW)up=held;
        if(code==EventKeyboard::KeyCode::KEY_DOWN_ARROW)down=held;
        if(held && code==EventKeyboard::KeyCode::KEY_ESCAPE) {
            UserDefault::getInstance()->setIntegerForKey("nextos-training-score",score);
            UserDefault::getInstance()->flush();Director::getInstance()->end();
        }
    };
    keyboard->onKeyPressed=[key](EventKeyboard::KeyCode k,Event*) {key(k,true);};
    keyboard->onKeyReleased=[key](EventKeyboard::KeyCode k,Event*) {key(k,false);};
    _eventDispatcher->addEventListenerWithSceneGraphPriority(keyboard,this);
    scheduleUpdate();return true;
}
void TrainingScene::update(float delta) {
    Vec2 motion((right?1.0f:0.0f)-(left?1.0f:0.0f),(up?1.0f:0.0f)-(down?1.0f:0.0f));
    if(motion.lengthSquared()>1)motion.normalize();
    position+=motion*(120*std::min(delta,0.25f));
    position.x=std::max(8.0f,std::min(632.0f,position.x));
    position.y=std::max(8.0f,std::min(472.0f,position.y));
    if(position.distance(Vec2(520,240))<12){score++;position=Vec2(40,240);}
    drawing->clear();drawing->drawSolidRect(Vec2::ZERO,Vec2(640,480),Color4F(0.08f,0.12f,0.18f,1));
    drawing->drawSolidRect(Vec2(512,232),Vec2(528,248),Color4F::YELLOW);
    drawing->drawSolidRect(position-Vec2(6,6),position+Vec2(6,6),Color4F(0.2f,0.8f,1,1));
}
